// WinFsp backend checks (docs/drive-redirection.md, "WinFsp backend"). Nothing is
// installed or mounted: the WinFsp exports are a fake table, the file-system
// callbacks are driven through the real FSP_FILE_SYSTEM_INTERFACE table against an
// in-memory PC (an NTFS-like drive-v1 client), the service<->helper wire/bridge/helper
// run over in-process pipes, the orchestrator uses a fake WinFsp mounter, and the
// installer logic uses a fake msiexec/registry.
#include "drive_redirection.hpp"
#include "drive_winfsp_helper.hpp"
#include "setup_winfsp.hpp"
#include <iostream>
#include <random>

using namespace host;
using namespace host::drive;

namespace {
int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; } } while (0)
#define CHECK_EQ(a, b) do { ++checks; const auto va = (a); const auto vb = (b); if (!(va == vb)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #a " == " #b " (" << va << " vs " << vb << ")\n"; } } while (0)
bool eventually(const std::function<bool()>& condition, int ms = 5000) { for (int i = 0; i < ms / 10; ++i) { if (condition()) return true; Sleep(10); } return condition(); }

// ---- the PC: an NTFS-like drive-v1 client with sparse files -------------------
std::string lowerKey(const std::string& path) { std::wstring w = wide(path); for (auto& c : w) c = wchar_t(towlower(c)); return utf8(w); }
class Mem final : public Backend {
 public:
  struct Node { std::string name; bool dir = false; uint64_t size = 0; std::map<uint64_t, std::array<uint8_t, 4096>> pages; int64_t modified = 1761300000000, created = 1761200000000; };
  std::mutex mutex; std::map<std::string, Node> nodes;  // lower-case path -> node ("" is the root)
  std::vector<Call> calls; std::map<std::string, std::string> fail;  // op name -> error code
  uint32_t page = 3; std::chrono::milliseconds delay{0}; std::atomic<int> inflight{0}, peak{0}; std::atomic<bool> hold{false};
  std::atomic<size_t> reads{0}, readBytes{0}, flushes{0}; bool space = true; std::mutex threadsMutex_; std::vector<std::thread> threads_;
  Mem() { nodes[""] = Node{"", true}; }
  static std::string leaf(const std::string& p) { const auto s = p.rfind('/'); return s == std::string::npos ? p : p.substr(s + 1); }
  static std::string parent(const std::string& p) { const auto s = p.rfind('/'); return s == std::string::npos ? std::string() : p.substr(0, s); }
  void file(const std::string& path, const std::string& text) { std::lock_guard lock(mutex); Node n; n.name = leaf(path); n.size = text.size(); store(n, 0, reinterpret_cast<const uint8_t*>(text.data()), text.size()); nodes[lowerKey(path)] = std::move(n); }
  void dir(const std::string& path) { std::lock_guard lock(mutex); Node n; n.name = leaf(path); n.dir = true; nodes[lowerKey(path)] = n; }
  void sparse(const std::string& path, uint64_t size) { std::lock_guard lock(mutex); Node n; n.name = leaf(path); n.size = size; nodes[lowerKey(path)] = std::move(n); }
  bool exists(const std::string& path) { std::lock_guard lock(mutex); return nodes.count(lowerKey(path)) != 0; }
  uint64_t sizeOf(const std::string& path) { std::lock_guard lock(mutex); return nodes[lowerKey(path)].size; }
  std::string content(const std::string& path) { std::lock_guard lock(mutex); const auto& n = nodes[lowerKey(path)]; std::string out(size_t(n.size), '\0'); load(n, 0, reinterpret_cast<uint8_t*>(out.data()), out.size()); return out; }
  std::string nameOf(const std::string& path) { std::lock_guard lock(mutex); return nodes[lowerKey(path)].name; }
  size_t count(Op op) { std::lock_guard lock(mutex); size_t n = 0; for (const auto& c : calls) n += c.op == op; return n; }
  std::future<Result> submit(Call call) override {
    if (!delay.count() && !hold) { std::promise<Result> p; p.set_value(run(call)); return p.get_future(); }
    // A promise-backed future (std::async's would block its caller on destruction, hiding the timeout paths under test).
    auto promise = std::make_shared<std::promise<Result>>(); auto future = promise->get_future();
    std::lock_guard lock(threadsMutex_);
    threads_.emplace_back([this, call, promise] {
      const int now = ++inflight; int seen = peak; while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
      while (hold) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      std::this_thread::sleep_for(delay);
      auto r = run(call); --inflight; promise->set_value(std::move(r));
    });
    return future;
  }
  ~Mem() override { hold = false; std::lock_guard lock(threadsMutex_); for (auto& t : threads_) if (t.joinable()) t.join(); }
 private:
  static void store(Node& n, uint64_t offset, const uint8_t* data, size_t size) { for (size_t i = 0; i < size;) { const uint64_t at = offset + i, page = at / 4096; const size_t inPage = size_t(at % 4096), take = std::min<size_t>(4096 - inPage, size - i); auto& p = n.pages[page]; memcpy(p.data() + inPage, data + i, take); i += take; } }
  static void load(const Node& n, uint64_t offset, uint8_t* out, size_t size) { memset(out, 0, size); for (size_t i = 0; i < size;) { const uint64_t at = offset + i, page = at / 4096; const size_t inPage = size_t(at % 4096), take = std::min<size_t>(4096 - inPage, size - i); const auto found = n.pages.find(page); if (found != n.pages.end()) memcpy(out + i, found->second.data() + inPage, take); i += take; } }
  Entry entry(const Node& n) const { Entry e; e.name = n.name; e.dir = n.dir; e.size = n.dir ? 0 : n.size; e.modified = n.modified; e.created = n.created; return e; }
  Result run(const Call& call) {
    std::lock_guard lock(mutex); calls.push_back(call);
    Result r; const auto err = [&](const char* code) { r.error = code; return r; };
    const auto f = fail.find(wire::opName(call.op)); if (f != fail.end()) { r.error = f->second; return r; }
    const std::string key = lowerKey(call.path); const auto found = nodes.find(key);
    switch (call.op) {
      case Op::stat: if (found == nodes.end()) { const auto up = nodes.find(lowerKey(parent(call.path))); return err(up != nodes.end() && !up->second.dir ? "drive_not_directory" : "drive_not_found"); } r.entry = entry(found->second); if (key.empty() && space) { r.entry.hasSpace = true; r.entry.free = 5000000; r.entry.total = 9000000; } return r;
      case Op::list: {
        if (found == nodes.end()) return err("drive_not_found"); if (!found->second.dir) return err("drive_not_directory");
        std::vector<Entry> all; for (const auto& [k, n] : nodes) if (!k.empty() && parent(k) == key) all.push_back(entry(n));
        const size_t from = call.cursor, to = std::min(all.size(), from + page);
        for (size_t i = from; i < to; ++i) r.entries.push_back(all[i]);
        if (to < all.size()) r.next = uint32_t(to);
        return r; }
      case Op::read: {
        if (found == nodes.end()) return err("drive_not_found"); if (found->second.dir) return err("drive_is_directory");
        ++reads; const auto& n = found->second; if (call.offset >= n.size) return r;
        const size_t take = size_t(std::min<uint64_t>(call.length, n.size - call.offset)); r.data.resize(take); load(n, call.offset, r.data.data(), take); readBytes += take; return r; }
      case Op::write: {
        auto it = nodes.find(key);
        if (it == nodes.end()) return err("drive_not_found");
        if (it->second.dir) return err("drive_is_directory");
        if (call.truncate) { it->second.pages.clear(); it->second.size = 0; }
        store(it->second, call.offset, call.data.data(), call.data.size()); it->second.size = std::max<uint64_t>(it->second.size, call.offset + call.data.size()); it->second.modified += 1000; return r; }
      case Op::create: {
        if (found != nodes.end()) return err("drive_exists");
        const auto p = nodes.find(lowerKey(parent(call.path))); if (p == nodes.end()) return err("drive_not_found"); if (!p->second.dir) return err("drive_not_directory");
        Node n; n.name = leaf(call.path); n.dir = call.dir; nodes[key] = n; return r; }
      case Op::rename: {
        if (found == nodes.end()) return err("drive_not_found");
        const std::string target = lowerKey(call.to); const auto dest = nodes.find(target);
        if (dest != nodes.end() && target != key) { if (!call.replace) return err("drive_exists"); if (dest->second.dir) return err("drive_is_directory"); nodes.erase(dest); }
        const auto p = nodes.find(lowerKey(parent(call.to))); if (p == nodes.end()) return err("drive_not_found");
        std::map<std::string, Node> moved; Node self = found->second; self.name = leaf(call.to);
        for (auto it = nodes.begin(); it != nodes.end();) { if (it->first.rfind(key + "/", 0) == 0) { Node child = it->second; moved[target + it->first.substr(key.size())] = child; it = nodes.erase(it); } else ++it; }
        nodes.erase(key); nodes[target] = self; for (auto& [k, v] : moved) nodes[k] = v; return r; }
      case Op::remove: {
        if (found == nodes.end()) return err("drive_not_found");
        if (found->second.dir) for (const auto& [k, n] : nodes) { (void)n; if (k.rfind(key + "/", 0) == 0) return err("drive_not_empty"); }
        nodes.erase(found); return r; }
      case Op::touch: if (found == nodes.end()) return err("drive_not_found"); found->second.modified = call.modified; return r;
      case Op::setsize: {
        if (found == nodes.end()) return err("drive_not_found"); if (found->second.dir) return err("drive_is_directory");
        auto& n = found->second;
        if (call.size < n.size) { for (auto it = n.pages.begin(); it != n.pages.end();) { if (it->first * 4096 >= call.size) it = n.pages.erase(it); else ++it; } const auto last = n.pages.find(call.size / 4096); if (last != n.pages.end()) memset(last->second.data() + call.size % 4096, 0, 4096 - call.size % 4096); }
        n.size = call.size; return r; }
      case Op::flush: if (found == nodes.end()) return err("drive_not_found"); ++flushes; return r;
    }
    return err("drive_io_failed");
  }
};

// ---- a fake WinFsp ---------------------------------------------------------------
struct DirBuf { std::vector<std::vector<uint8_t>> entries; };
struct FakeFsp {
  std::mutex mutex; std::vector<std::string> events; FSP_FSCTL_VOLUME_PARAMS params{}; std::wstring device; const FSP_FILE_SYSTEM_INTERFACE* table = nullptr; FSP_FILE_SYSTEM* fs = nullptr;
  NTSTATUS createStatus = 0, startStatus = 0, mountStatus = 0; wchar_t letter = L'Z'; wchar_t mountPoint[3]{}; int created = 0, deleted = 0;
  std::function<void()> beforeMount;
  static FakeFsp*& current() { static FakeFsp* value = nullptr; return value; }
  fs::Api api() {
    current() = this; fs::Api a;
    a.create = [](PWSTR d, const FSP_FSCTL_VOLUME_PARAMS* p, const FSP_FILE_SYSTEM_INTERFACE* t, FSP_FILE_SYSTEM** out) -> NTSTATUS {
      auto& s = *current(); std::lock_guard lock(s.mutex); if (s.createStatus) return s.createStatus; s.events.push_back("create"); s.device = d; s.params = *p; s.table = t; ++s.created;
      s.fs = static_cast<FSP_FILE_SYSTEM*>(calloc(1, sizeof(FSP_FILE_SYSTEM))); *out = s.fs; return 0; };
    a.destroy = [](FSP_FILE_SYSTEM* f) { auto& s = *current(); std::lock_guard lock(s.mutex); s.events.push_back("delete"); ++s.deleted; free(f); if (s.fs == f) s.fs = nullptr; };
    a.setMountPoint = [](FSP_FILE_SYSTEM* f, PWSTR m) -> NTSTATUS { auto& s = *current(); if (s.beforeMount) s.beforeMount(); std::lock_guard lock(s.mutex); if (s.mountStatus) return s.mountStatus; s.events.push_back(std::string("mount:") + (m[0] == L'*' ? "auto" : "fixed")); s.mountPoint[0] = s.letter; s.mountPoint[1] = L':'; f->MountPoint = s.mountPoint; return 0; };
    a.removeMountPoint = [](FSP_FILE_SYSTEM*) { auto& s = *current(); std::lock_guard lock(s.mutex); s.events.push_back("unmount"); };
    a.startDispatcher = [](FSP_FILE_SYSTEM*, ULONG n) -> NTSTATUS { auto& s = *current(); std::lock_guard lock(s.mutex); if (s.startStatus) return s.startStatus; s.events.push_back("start:" + std::to_string(n)); return 0; };
    a.stopDispatcher = [](FSP_FILE_SYSTEM*) { auto& s = *current(); std::lock_guard lock(s.mutex); s.events.push_back("stop"); };
    a.acquireDirectoryBuffer = [](PVOID* b, BOOLEAN reset, PNTSTATUS st) -> BOOLEAN { *st = 0; if (!*b) { *b = new DirBuf; return TRUE; } if (reset) { static_cast<DirBuf*>(*b)->entries.clear(); return TRUE; } return FALSE; };
    a.fillDirectoryBuffer = [](PVOID* b, FSP_FSCTL_DIR_INFO* info, PNTSTATUS) -> BOOLEAN { static_cast<DirBuf*>(*b)->entries.emplace_back(reinterpret_cast<uint8_t*>(info), reinterpret_cast<uint8_t*>(info) + info->Size); return TRUE; };
    a.releaseDirectoryBuffer = [](PVOID*) {};
    a.readDirectoryBuffer = [](PVOID* b, PWSTR marker, PVOID buffer, ULONG length, PULONG done) {
      auto& list = static_cast<DirBuf*>(*b)->entries;
      const auto name = [](const std::vector<uint8_t>& e) { const auto* i = reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(e.data()); std::wstring n(reinterpret_cast<const wchar_t*>(i->FileNameBuf), (i->Size - sizeof(FSP_FSCTL_DIR_INFO)) / 2); return n; };
      std::sort(list.begin(), list.end(), [&](const auto& x, const auto& y) { return _wcsicmp(name(x).c_str(), name(y).c_str()) < 0; });
      ULONG used = 0; for (const auto& e : list) { if (marker && _wcsicmp(name(e).c_str(), marker) <= 0) continue; const ULONG size = (ULONG(e.size()) + 7) & ~7u; if (used + size > length) break; memset(static_cast<uint8_t*>(buffer) + used, 0, size); memcpy(static_cast<uint8_t*>(buffer) + used, e.data(), e.size()); used += size; }
      *done = used; };
    a.deleteDirectoryBuffer = [](PVOID* b) { delete static_cast<DirBuf*>(*b); *b = nullptr; };
    a.version = [](PUINT32 v) -> NTSTATUS { *v = 0x00020001; return 0; };
    return a;
  }
  std::vector<std::string> log() { std::lock_guard lock(mutex); return events; }
};
// Calls through the interface table WinFsp would use.
struct Vol {
  FSP_FILE_SYSTEM* fs; const FSP_FILE_SYSTEM_INTERFACE* t;
  explicit Vol(FakeFsp& f) : fs(f.fs), t(f.table) {}
  NTSTATUS open(const wchar_t* n, PVOID* c, FSP_FSCTL_FILE_INFO* i) { return t->Open(fs, const_cast<PWSTR>(n), 0, 0, c, i); }
  NTSTATUS create(const wchar_t* n, bool dir, PVOID* c, FSP_FSCTL_FILE_INFO* i) { return t->Create(fs, const_cast<PWSTR>(n), dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE, 0, 0, nullptr, 0, c, i); }
  NTSTATUS sec(const wchar_t* n, UINT32* attr) { std::vector<uint8_t> sd(512); SIZE_T size = sd.size(); return t->GetSecurityByName(fs, const_cast<PWSTR>(n), attr, sd.data(), &size); }
  NTSTATUS read(PVOID c, std::string& out, UINT64 off, ULONG len) { out.assign(len, '\0'); ULONG done = 0; const NTSTATUS s = t->Read(fs, c, out.data(), off, len, &done); out.resize(done); return s; }
  NTSTATUS write(PVOID c, const std::string& data, UINT64 off, FSP_FSCTL_FILE_INFO* i, bool end = false, bool constrained = false, ULONG* done = nullptr) { ULONG d = 0; std::string copy = data; const NTSTATUS s = t->Write(fs, c, copy.data(), off, ULONG(copy.size()), end, constrained, &d, i); if (done) *done = d; return s; }
  void cleanup(PVOID c, const wchar_t* n, ULONG flags) { t->Cleanup(fs, c, const_cast<PWSTR>(n), flags); }
  void close(PVOID c) { t->Close(fs, c); }
  std::vector<std::wstring> list(PVOID c, bool* ok = nullptr, ULONG capacity = 1 << 16) {
    std::vector<std::wstring> names; std::vector<uint8_t> buffer(capacity); std::wstring marker; bool first = true;
    for (int rounds = 0; rounds < 200; ++rounds) {
      ULONG used = 0; const NTSTATUS s = t->ReadDirectory(fs, c, nullptr, first ? nullptr : marker.data(), buffer.data(), ULONG(buffer.size()), &used); first = false;
      if (s) { if (ok) *ok = false; return names; }
      if (!used) break;
      for (ULONG at = 0; at < used;) { const auto* info = reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(buffer.data() + at); if (!info->Size) break; names.emplace_back(reinterpret_cast<const wchar_t*>(info->FileNameBuf), (info->Size - sizeof(FSP_FSCTL_DIR_INFO)) / 2); marker = names.back(); at += (info->Size + 7) & ~7u; }
    }
    if (ok) *ok = true; return names;
  }
};
std::wstring currentSid() { return fs::currentUserSid(); }
fs::Options options(const std::string& id = "docs", bool readOnly = false) {
  fs::Options o; o.shareId = id; o.token = std::string(sharePrefix) + randomToken(); o.label = L"Documents (PC)"; o.readOnly = readOnly; o.ownerSid = currentSid(); o.callTimeout = std::chrono::milliseconds(3000); o.dispatcherThreads = 4; return o;
}

// ---- pure rules ------------------------------------------------------------------
void rules() {
  CHECK(fs::toDrivePath(L"\\") == std::optional<std::string>(""));
  CHECK(fs::toDrivePath(L"\\a") == std::optional<std::string>("a"));
  CHECK(fs::toDrivePath(L"\\a\\b\\c.txt") == std::optional<std::string>("a/b/c.txt"));
  CHECK(fs::toDrivePath(L"\\a\\b\\") == std::optional<std::string>("a/b"));
  CHECK(fs::toDrivePath(L"\\\uD55C\uAE00 \uD3F4\uB354\\\uD30C\uC77C.txt") == std::optional<std::string>(utf8(L"\uD55C\uAE00 \uD3F4\uB354/\uD30C\uC77C.txt")));
  CHECK(fs::toDrivePath(L"\\\U0001F600\\x") == std::optional<std::string>(utf8(L"\U0001F600/x")));
  for (const wchar_t* bad : {L"a\\b", L"", L"\\a:stream", L"\\a\\b:$DATA", L"\\CON", L"\\a\\nul.txt", L"\\a.", L"\\a ", L"\\a\\..", L"\\a\\.", L"\\a?b", L"\\a*", L"\\a|b", L"\\a<b", L"\\a\"b"})
    CHECK(!fs::toDrivePath(bad));
  CHECK(!fs::toDrivePath(nullptr));
  CHECK(!fs::toDrivePath((L"\\" + std::wstring(256, L'x')).c_str()));   // component above 255
  CHECK(fs::toDrivePath((L"\\" + std::wstring(255, L'x')).c_str()));
  std::wstring deep; for (int i = 0; i < 64; ++i) deep += L"\\d"; CHECK(fs::toDrivePath(deep.c_str()));
  deep += L"\\d"; CHECK(!fs::toDrivePath(deep.c_str()));            // 65 levels
  std::wstring longPath; while (longPath.size() < 1030) longPath += L"\\" + std::wstring(100, L'y'); CHECK(!fs::toDrivePath(longPath.c_str()));
  // Long paths below the drive-v1 bound work (6 levels, > 300 characters).
  std::wstring ok; for (int i = 0; i < 6; ++i) ok += L"\\" + std::wstring(60, wchar_t(L'a' + i)); CHECK(ok.size() > 300 && fs::toDrivePath(ok.c_str()));
  CHECK_EQ(fs::statusFor("drive_not_found"), STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK_EQ(fs::statusFor("drive_not_found", true), STATUS_FILE_INVALID);
  CHECK_EQ(fs::statusFor("drive_exists"), STATUS_OBJECT_NAME_COLLISION);
  CHECK_EQ(fs::statusFor("drive_not_empty"), STATUS_DIRECTORY_NOT_EMPTY);
  CHECK_EQ(fs::statusFor("drive_read_only"), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK_EQ(fs::statusFor("drive_denied"), STATUS_ACCESS_DENIED);
  CHECK_EQ(fs::statusFor("drive_link_refused"), STATUS_ACCESS_DENIED);
  CHECK_EQ(fs::statusFor("drive_unavailable"), STATUS_NETWORK_NAME_DELETED);
  CHECK_EQ(fs::statusFor("drive_share_unknown"), STATUS_NETWORK_NAME_DELETED);
  CHECK_EQ(fs::statusFor("drive_busy"), STATUS_SHARING_VIOLATION);
  CHECK_EQ(fs::statusFor("drive_no_space"), STATUS_DISK_FULL);
  CHECK_EQ(fs::statusFor("drive_is_directory"), STATUS_FILE_IS_A_DIRECTORY);
  CHECK_EQ(fs::statusFor("drive_not_directory"), STATUS_NOT_A_DIRECTORY);
  CHECK_EQ(fs::statusFor("drive_timeout"), STATUS_IO_TIMEOUT);
  CHECK_EQ(fs::statusFor("drive_io_failed"), STATUS_UNEXPECTED_IO_ERROR);
  CHECK_EQ(fs::statusFor("something_else"), STATUS_UNEXPECTED_IO_ERROR);
  CHECK_EQ(fs::statusFor(""), STATUS_SUCCESS);
  CHECK_EQ(fs::toUnixMs(fs::toFileTime(1761300000123)), 1761300000123);
  CHECK_EQ(fs::toUnixMs(0), 0); CHECK_EQ(fs::allocationFor(0), 0u); CHECK_EQ(fs::allocationFor(1), 4096u); CHECK_EQ(fs::allocationFor(4096), 4096u); CHECK_EQ(fs::allocationFor(4097), 8192u);
  CHECK(fs::indexNumber("a/b") != fs::indexNumber("a/c") && fs::indexNumber("") != 0);
}

// ---- the wire ---------------------------------------------------------------------
void wireChecks() {
  using namespace wire;
  std::vector<Call> calls;
  { Call c; c.op = Op::stat; c.share = "a1"; c.path = ""; calls.push_back(c); }
  { Call c; c.op = Op::list; c.share = "a1"; c.path = "d"; c.cursor = 7; calls.push_back(c); }
  { Call c; c.op = Op::read; c.share = "a1"; c.path = "f/g.bin"; c.offset = 5368709120ULL; c.length = 262144; calls.push_back(c); }
  { Call c; c.op = Op::write; c.share = "a1"; c.path = "f"; c.offset = 4294967296ULL; c.truncate = false; c.data = {1, 2, 3}; calls.push_back(c); }
  { Call c; c.op = Op::write; c.share = "a1"; c.path = "f"; c.truncate = true; c.data = {9}; calls.push_back(c); }
  { Call c; c.op = Op::create; c.share = "a1"; c.path = "n"; c.dir = true; calls.push_back(c); }
  { Call c; c.op = Op::rename; c.share = "a1"; c.path = "n"; c.to = "m"; c.replace = true; calls.push_back(c); }
  { Call c; c.op = Op::remove; c.share = "a1"; c.path = "n"; calls.push_back(c); }
  { Call c; c.op = Op::touch; c.share = "a1"; c.path = "n"; c.modified = 1761300000000; calls.push_back(c); }
  { Call c; c.op = Op::setsize; c.share = "a1"; c.path = "n"; c.size = 9007199254740991ULL; calls.push_back(c); }
  { Call c; c.op = Op::flush; c.share = "a1"; c.path = "n"; calls.push_back(c); }
  uint32_t id = 1;
  for (const auto& call : calls) {
    const auto bytes = encode(callFrame(id, call)); wire::Frame frame; size_t at = 0;
    CHECK(readFrame([&](uint8_t* b, size_t n) { const size_t take = std::min(n, bytes.size() - at); memcpy(b, bytes.data() + at, take); at += take; return take; }, frame));
    Call parsed; CHECK(callParse(frame, parsed));
    CHECK(parsed.op == call.op && parsed.share == call.share && parsed.path == call.path && parsed.to == call.to && parsed.offset == call.offset && parsed.length == call.length && parsed.cursor == call.cursor && parsed.dir == call.dir && parsed.truncate == call.truncate && parsed.replace == call.replace && parsed.modified == call.modified && parsed.size == call.size && parsed.data == call.data);
    CHECK_EQ(frame.id, id); ++id;
  }
  // The service never trusts the helper beyond drive-v1.
  const auto reject = [](const std::string& header, const std::vector<uint8_t>& data = {}, uint32_t frameId = 5) { wire::Frame f; f.kind = Kind::call; f.id = frameId; f.header = header; f.data = data; Call c; return !callParse(f, c); };
  CHECK(reject("{\"op\":\"format\",\"share\":\"a\",\"path\":\"\"}"));
  CHECK(reject("{\"op\":\"stat\",\"share\":\"bad id\",\"path\":\"\"}"));
  CHECK(reject("{\"op\":\"stat\",\"share\":\"a\",\"path\":\"../x\"}"));
  CHECK(reject("{\"op\":\"stat\",\"share\":\"a\",\"path\":\"a:b\"}"));
  CHECK(reject("{\"op\":\"stat\",\"share\":\"a\",\"path\":\"\"}", {1}));                         // data only on write
  CHECK(reject("{\"op\":\"stat\",\"share\":\"a\",\"path\":\"\"}", {}, 0));                       // id 0 is control
  CHECK(reject("{\"op\":\"read\",\"share\":\"a\",\"path\":\"f\",\"offset\":0,\"length\":262145}"));
  CHECK(reject("{\"op\":\"read\",\"share\":\"a\",\"path\":\"f\",\"offset\":0,\"length\":0}"));
  CHECK(reject("{\"op\":\"read\",\"share\":\"a\",\"path\":\"f\",\"offset\":-1,\"length\":1}"));
  CHECK(reject("{\"op\":\"read\",\"share\":\"a\",\"path\":\"f\",\"offset\":9007199254740992,\"length\":1}"));
  CHECK(reject("{\"op\":\"write\",\"share\":\"a\",\"path\":\"f\",\"offset\":0,\"truncate\":false}"));         // empty data
  CHECK(reject("{\"op\":\"write\",\"share\":\"a\",\"path\":\"f\",\"offset\":5,\"truncate\":true}", {1}));    // truncate only at 0
  CHECK(reject("{\"op\":\"write\",\"share\":\"a\",\"path\":\"\",\"offset\":0,\"truncate\":true}", {1}));      // root
  CHECK(reject("{\"op\":\"write\",\"share\":\"a\",\"path\":\"f\",\"offset\":0,\"truncate\":false}", std::vector<uint8_t>(262145, 1)));
  CHECK(reject("{\"op\":\"create\",\"share\":\"a\",\"path\":\"f\",\"dir\":true,\"truncate\":true}"));
  CHECK(reject("{\"op\":\"create\",\"share\":\"a\",\"path\":\"\",\"dir\":false,\"truncate\":false}"));
  CHECK(reject("{\"op\":\"rename\",\"share\":\"a\",\"path\":\"f\",\"to\":\"f\",\"replace\":false}"));
  CHECK(reject("{\"op\":\"rename\",\"share\":\"a\",\"path\":\"f\",\"to\":\"\",\"replace\":false}"));
  CHECK(reject("{\"op\":\"delete\",\"share\":\"a\",\"path\":\"\"}"));
  CHECK(reject("{\"op\":\"touch\",\"share\":\"a\",\"path\":\"f\",\"modified\":-1}"));
  CHECK(reject("{\"op\":\"setsize\",\"share\":\"a\",\"path\":\"\",\"size\":1}"));
  CHECK(reject("{\"op\":\"setsize\",\"share\":\"a\",\"path\":\"f\",\"size\":9007199254740992}"));
  CHECK(reject("{\"op\":\"setsize\",\"share\":\"a\",\"path\":\"f\"}"));
  CHECK(reject("{not json"));
  // Results.
  Result r; r.entry.name = "x"; r.entry.size = 5368709120ULL; r.entry.modified = 5; r.entry.created = 4; r.entries.push_back(r.entry); Entry root; root.dir = true; root.hasSpace = true; root.free = 1; root.total = 2; r.entries.push_back(root); r.next = 9; r.data = {7, 8};
  wire::Frame rf = resultFrame(77, r); Result back; CHECK(resultParse(rf, back));
  CHECK(back.ok() && back.entry.name == "x" && back.entry.size == 5368709120ULL && back.entries.size() == 2 && back.entries[1].hasSpace && back.entries[1].total == 2 && back.next == std::optional<uint32_t>(9) && back.data == r.data);
  Result bad; bad.error = "drive_not_found"; Result badBack; CHECK(resultParse(resultFrame(1, bad), badBack) && badBack.error == "drive_not_found");
  wire::Frame broken; broken.kind = Kind::result; broken.header = "{\"ok\":true,\"entry\":[1,2]}"; Result unused; CHECK(!resultParse(broken, unused));
  // Frames: truncated / oversize / unknown kind end the stream.
  { const auto good = encode(controlFrame("{\"type\":\"stop\"}")); size_t at = 0; wire::Frame f; CHECK(!readFrame([&](uint8_t* b, size_t n) { const size_t take = std::min(n, good.size() - 3 - at); memcpy(b, good.data() + at, take); at += take; return take; }, f)); }
  { std::vector<uint8_t> huge{0xff, 0xff, 0xff, 0x7f}; size_t at = 0; wire::Frame f; CHECK(!readFrame([&](uint8_t* b, size_t n) { const size_t take = std::min(n, huge.size() - at); memcpy(b, huge.data() + at, take); at += take; return take; }, f)); }
  { auto odd = encode(controlFrame("{}")); odd[4] = 9; size_t at = 0; wire::Frame f; CHECK(!readFrame([&](uint8_t* b, size_t n) { const size_t take = std::min(n, odd.size() - at); memcpy(b, odd.data() + at, take); at += take; return take; }, f)); }
}

// ---- the file system -----------------------------------------------------------------
void mountAndSecurity() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api();
  {
    auto o = options(); const auto token = o.token; fs::ShareFs share(api, mem, o);
    std::wstring letter; CHECK_EQ(share.mount(letter), 0); CHECK(letter == L"Z");
    const auto log = fsp.log();
    CHECK(log.size() == 3 && log[0] == "create" && log[1] == "start:4" && log[2] == "mount:auto");            // dispatcher before the mount point
    CHECK(fsp.device == L"WinFsp.Net");                                                                         // a network volume: no recycle bin, no indexing
    CHECK(fsp.params.Prefix == std::wstring(L"\\multitap\\") + wide(token) && std::wstring(fsp.params.FileSystemName) == L"Multitap");
    CHECK(fsp.params.FlushAndPurgeOnCleanup && fsp.params.PersistentAcls && fsp.params.CasePreservedNames && !fsp.params.CaseSensitiveSearch && fsp.params.UnicodeOnDisk && !fsp.params.ReparsePoints && !fsp.params.NamedStreams && !fsp.params.ExtendedAttributes && !fsp.params.ReadOnlyVolume);
    CHECK_EQ(fsp.params.MaxComponentLength, 255); CHECK_EQ(fsp.params.FileInfoTimeout, 1000u); CHECK(fsp.params.IrpTimeout >= 90000 && fsp.params.SectorSize == 4096);
    CHECK(share.mount(letter) == STATUS_INVALID_PARAMETER);                                                     // once
    // The drive belongs to this logon session only: the process is not a service, so WinFsp uses DefineDosDevice itself.
    Vol v(fsp);
    FSP_FSCTL_VOLUME_INFO info{}; CHECK_EQ(v.t->GetVolumeInfo(fsp.fs, &info), 0);
    CHECK(info.TotalSize == 9000000 && info.FreeSize == 5000000 && std::wstring(info.VolumeLabel, info.VolumeLabelLength / 2) == L"Documents (PC)");
    mem.space = false; share.setReadOnly(false); FSP_FSCTL_VOLUME_INFO unknown{}; CHECK_EQ(v.t->GetVolumeInfo(fsp.fs, &unknown), 0); CHECK(unknown.FreeSize > (1ULL << 39));   // unknown space does not look full
    share.unmount();
    const auto after = fsp.log();
    CHECK(after.size() == 6 && after[3] == "unmount" && after[4] == "stop" && after[5] == "delete");           // drive first, then the dispatcher, then the volume
    share.unmount(); CHECK_EQ(fsp.deleted, 1);
  }
  // Failures leave nothing behind.
  { FakeFsp bad; bad.createStatus = STATUS_ACCESS_DENIED; Mem m; fs::ShareFs s(bad.api(), m, options()); std::wstring l; CHECK_EQ(s.mount(l), STATUS_ACCESS_DENIED); CHECK(l.empty() && bad.created == 0); }
  { FakeFsp bad; bad.startStatus = STATUS_INSUFFICIENT_RESOURCES; Mem m; fs::ShareFs s(bad.api(), m, options()); std::wstring l; CHECK_EQ(s.mount(l), STATUS_INSUFFICIENT_RESOURCES); CHECK_EQ(bad.deleted, 1); }
  { FakeFsp bad; bad.mountStatus = STATUS_NO_SUCH_DEVICE; Mem m; fs::ShareFs s(bad.api(), m, options()); std::wstring l; CHECK_EQ(s.mount(l), STATUS_NO_SUCH_DEVICE); const auto log = bad.log(); CHECK(log.size() >= 4 && log[log.size() - 2] == "stop" && log.back() == "delete"); }
  { fs::Api incomplete; Mem m; fs::ShareFs s(incomplete, m, options()); std::wstring l; CHECK_EQ(s.mount(l), STATUS_NOT_SUPPORTED); }
  { auto o = options(); o.ownerSid.clear(); Mem m; FakeFsp f; fs::ShareFs s(f.api(), m, o); std::wstring l; CHECK(!s.hasSecurity() && s.mount(l) == STATUS_NOT_SUPPORTED); }   // fail closed without an owner
}

void securityDescriptor() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring l; CHECK_EQ(share.mount(l), 0);
  mem.file("a.txt", "hi"); Vol v(fsp);
  std::vector<uint8_t> sd(1024); SIZE_T size = 4; UINT32 attr = 0;
  CHECK_EQ(v.t->GetSecurityByName(fsp.fs, const_cast<PWSTR>(L"\\a.txt"), &attr, sd.data(), &size), STATUS_BUFFER_OVERFLOW); CHECK(size > 20 && size < 1024);
  size = sd.size(); CHECK_EQ(v.t->GetSecurityByName(fsp.fs, const_cast<PWSTR>(L"\\a.txt"), &attr, sd.data(), &size), 0);
  CHECK(IsValidSecurityDescriptor(sd.data()));
  CHECK(attr == (FILE_ATTRIBUTE_ARCHIVE));
  BOOL present = FALSE, defaulted = FALSE; PACL acl = nullptr; CHECK(GetSecurityDescriptorDacl(sd.data(), &present, &acl, &defaulted) && present && acl);
  SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0; GetSecurityDescriptorControl(sd.data(), &control, &revision); CHECK((control & SE_DACL_PROTECTED) != 0);
  std::set<std::wstring> sids; for (DWORD i = 0; i < acl->AceCount; ++i) { void* ace = nullptr; GetAce(acl, i, &ace); auto* a = static_cast<ACCESS_ALLOWED_ACE*>(ace); CHECK_EQ(a->Header.AceType, ACCESS_ALLOWED_ACE_TYPE); LPWSTR text = nullptr; ConvertSidToStringSidW(reinterpret_cast<PSID>(&a->SidStart), &text); sids.insert(text); LocalFree(text); }
  CHECK(sids == (std::set<std::wstring>{currentSid(), L"S-1-5-18", L"S-1-5-32-544"}));      // owner, SYSTEM, Administrators: nobody else, not Everyone
  PSID owner = nullptr; BOOL ownerDefaulted = FALSE; GetSecurityDescriptorOwner(sd.data(), &owner, &ownerDefaulted); LPWSTR ownerText = nullptr; ConvertSidToStringSidW(owner, &ownerText); CHECK(std::wstring(ownerText) == currentSid()); LocalFree(ownerText);
  // GetSecurity / SetSecurity: the same descriptor; changes are accepted and not stored.
  PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{}; CHECK_EQ(v.open(L"\\a.txt", &ctx, &info), 0);
  size = sd.size(); CHECK_EQ(v.t->GetSecurity(fsp.fs, ctx, sd.data(), &size), 0); CHECK_EQ(v.t->SetSecurity(fsp.fs, ctx, DACL_SECURITY_INFORMATION, sd.data()), 0);
  v.close(ctx); share.unmount();
}

void basicFlow() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.dir("docs"); mem.file("docs/readme.txt", "hello world");
  UINT32 attr = 0; CHECK_EQ(v.sec(L"\\", &attr), 0); CHECK(attr == FILE_ATTRIBUTE_DIRECTORY);
  CHECK_EQ(v.sec(L"\\docs\\readme.txt", &attr), 0); CHECK(attr == FILE_ATTRIBUTE_ARCHIVE);
  CHECK_EQ(v.sec(L"\\docs\\README.TXT", &attr), 0);                                         // case-insensitive
  CHECK_EQ(v.sec(L"\\nothing", &attr), STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK_EQ(v.sec(L"\\a:stream", &attr), STATUS_OBJECT_NAME_INVALID);
  CHECK_EQ(v.sec(L"\\docs\\readme.txt\\x", &attr), STATUS_OBJECT_PATH_NOT_FOUND);              // a file used as a folder
  PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.open(L"\\docs\\readme.txt", &ctx, &info), 0);
  CHECK(info.FileSize == 11 && info.AllocationSize == 4096 && (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && info.LastWriteTime == fs::toFileTime(1761300000000) && info.CreationTime == fs::toFileTime(1761200000000) && info.IndexNumber != 0);
  std::string text; CHECK_EQ(v.read(ctx, text, 0, 100), 0); CHECK(text == "hello world");
  CHECK_EQ(v.read(ctx, text, 6, 5), 0); CHECK(text == "world");
  CHECK_EQ(v.read(ctx, text, 11, 5), STATUS_END_OF_FILE); CHECK_EQ(v.read(ctx, text, 1000, 5), STATUS_END_OF_FILE);
  CHECK_EQ(v.read(ctx, text, 0, 0), 0);
  FSP_FSCTL_FILE_INFO again{}; CHECK_EQ(v.t->GetFileInfo(fsp.fs, ctx, &again), 0); CHECK(again.FileSize == 11);
  v.cleanup(ctx, L"\\docs\\readme.txt", 0); v.close(ctx);
  // A directory handle refuses file I/O, a file refuses directory listing.
  CHECK_EQ(v.open(L"\\docs", &ctx, &info), 0); CHECK(info.FileAttributes == FILE_ATTRIBUTE_DIRECTORY && info.FileSize == 0); CHECK_EQ(v.read(ctx, text, 0, 4), STATUS_FILE_IS_A_DIRECTORY); v.close(ctx);
  CHECK_EQ(v.open(L"\\docs\\readme.txt", &ctx, &info), 0); ULONG used = 0; std::vector<uint8_t> buffer(512); CHECK_EQ(v.t->ReadDirectory(fsp.fs, ctx, nullptr, nullptr, buffer.data(), 512, &used), STATUS_NOT_A_DIRECTORY); v.close(ctx);
  CHECK_EQ(v.open(L"\\missing.txt", &ctx, &info), STATUS_OBJECT_NAME_NOT_FOUND);
  // Create + write + flush on close.
  CHECK_EQ(v.create(L"\\docs\\new.txt", false, &ctx, &info), 0); CHECK(mem.exists("docs/new.txt") && info.FileSize == 0);
  FSP_FSCTL_FILE_INFO winfo{}; CHECK_EQ(v.write(ctx, "abcdef", 0, &winfo), 0); CHECK_EQ(winfo.FileSize, 6u);
  CHECK_EQ(v.write(ctx, "XY", 2, &winfo), 0); CHECK(mem.content("docs/new.txt") == "abXYef" && winfo.FileSize == 6);
  ULONG done = 0; CHECK_EQ(v.write(ctx, "++", 0, &winfo, true, false, &done), 0); CHECK(mem.content("docs/new.txt") == "abXYef++" && done == 2);           // write to end
  CHECK_EQ(v.write(ctx, "zzzzzz", 6, &winfo, false, true, &done), 0); CHECK(done == 2 && mem.content("docs/new.txt") == "abXYefzz");                    // constrained: not past the end
  CHECK_EQ(v.write(ctx, "q", 100, &winfo, false, true, &done), 0); CHECK(done == 0);
  CHECK_EQ(mem.flushes.load(), 0u);
  v.cleanup(ctx, L"\\docs\\new.txt", 0); CHECK_EQ(mem.flushes.load(), 1u);                  // the save is on the PC when the program closes the file
  v.cleanup(ctx, L"\\docs\\new.txt", 0); CHECK_EQ(mem.flushes.load(), 1u);                  // nothing dirty: no second flush
  v.close(ctx);
  CHECK_EQ(v.create(L"\\docs\\new.txt", false, &ctx, &info), STATUS_OBJECT_NAME_COLLISION);
  CHECK_EQ(v.create(L"\\nodir\\x.txt", false, &ctx, &info), STATUS_OBJECT_PATH_NOT_FOUND);
  CHECK_EQ(v.create(L"\\docs\\sub", true, &ctx, &info), 0); CHECK((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && mem.exists("docs/sub")); v.close(ctx);
  // Explicit Flush.
  CHECK_EQ(v.open(L"\\docs\\new.txt", &ctx, &info), 0); CHECK_EQ(v.write(ctx, "1", 0, &winfo), 0);
  FSP_FSCTL_FILE_INFO finfo{}; CHECK_EQ(v.t->Flush(fsp.fs, ctx, &finfo), 0); CHECK_EQ(mem.flushes.load(), 2u); CHECK_EQ(v.t->Flush(fsp.fs, nullptr, &finfo), 0); v.cleanup(ctx, L"\\docs\\new.txt", 0); CHECK_EQ(mem.flushes.load(), 2u);
  v.close(ctx);
  // Times: the last write time reaches the PC, other times are accepted and ignored.
  CHECK_EQ(v.open(L"\\docs\\new.txt", &ctx, &info), 0);
  CHECK_EQ(v.t->SetBasicInfo(fsp.fs, ctx, FILE_ATTRIBUTE_HIDDEN, 5, 6, fs::toFileTime(1700000000000), 8, &info), 0); CHECK(mem.nodes[lowerKey("docs/new.txt")].modified == 1700000000000);
  CHECK_EQ(v.t->SetBasicInfo(fsp.fs, ctx, 0, 0, 0, 0, 0, &info), 0); CHECK(mem.nodes[lowerKey("docs/new.txt")].modified == 1700000000000);
  v.close(ctx);
  share.unmount();
}

void sizesAndTruncation() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.file("f.bin", "0123456789"); PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.open(L"\\f.bin", &ctx, &info), 0);
  CHECK_EQ(v.t->SetFileSize(fsp.fs, ctx, 4, FALSE, &info), 0); CHECK(mem.content("f.bin") == "0123" && info.FileSize == 4);
  CHECK_EQ(v.t->SetFileSize(fsp.fs, ctx, 8, FALSE, &info), 0); CHECK(mem.sizeOf("f.bin") == 8 && mem.content("f.bin") == std::string("0123") + std::string(4, '\0'));
  // Allocation sizes are promises: larger never extends, smaller shortens.
  CHECK_EQ(v.t->SetFileSize(fsp.fs, ctx, 1 << 20, TRUE, &info), 0); CHECK(mem.sizeOf("f.bin") == 8 && info.AllocationSize >= (1 << 20) && info.FileSize == 8);
  CHECK_EQ(v.t->SetFileSize(fsp.fs, ctx, 2, TRUE, &info), 0); CHECK(mem.sizeOf("f.bin") == 2);
  CHECK_EQ(v.t->Overwrite(fsp.fs, ctx, FILE_ATTRIBUTE_NORMAL, TRUE, 0, &info), 0); CHECK(mem.sizeOf("f.bin") == 0 && info.FileSize == 0);
  // More than 4 GiB: a sparse 6 GiB file, a ranged read near its end, a write above 4 GiB.
  v.close(ctx); mem.sparse("big.bin", 6ULL << 30); const size_t before = mem.reads;
  CHECK_EQ(v.open(L"\\big.bin", &ctx, &info), 0); CHECK_EQ(info.FileSize, 6ULL << 30); CHECK_EQ(info.AllocationSize, 6ULL << 30);
  CHECK_EQ(v.write(ctx, "TAIL", (5ULL << 30) + 100, &info), 0);
  std::string text; CHECK_EQ(v.read(ctx, text, (5ULL << 30) + 98, 8), 0); CHECK(text == std::string(2, '\0') + "TAIL" + std::string(2, '\0'));
  CHECK(mem.reads - before == 1 && mem.readBytes <= 262144);                                      // one block, not the file
  CHECK_EQ(v.read(ctx, text, 6ULL << 30, 8), STATUS_END_OF_FILE);
  CHECK_EQ(v.t->SetFileSize(fsp.fs, ctx, 5ULL << 30, FALSE, &info), 0); CHECK_EQ(mem.sizeOf("big.bin"), 5ULL << 30); CHECK_EQ(info.FileSize, 5ULL << 30);
  v.close(ctx);
  { Mem full; FakeFsp f2; fs::ShareFs s2(f2.api(), full, options()); std::wstring l2; s2.mount(l2); Vol w(f2); full.file("x", "1"); full.fail["setsize"] = "drive_no_space"; PVOID c = nullptr; FSP_FSCTL_FILE_INFO i{}; w.open(L"\\x", &c, &i); CHECK_EQ(w.t->SetFileSize(f2.fs, c, 99, FALSE, &i), STATUS_DISK_FULL); w.close(c); }
  share.unmount();
}

void readAhead() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); auto o = options(); o.cacheBlocks = 64; fs::ShareFs share(api, mem, o); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.sparse("movie.bin", 40ULL << 20); PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{}; CHECK_EQ(v.open(L"\\movie.bin", &ctx, &info), 0);
  std::string text;
  // Random 4 KiB reads (memory-mapped paging): one 256 KiB block each, repeats come from the cache.
  for (uint64_t offset : {uint64_t(1) << 20, (uint64_t(1) << 20) + 100000, (uint64_t(1) << 20) + 200000, uint64_t(9) << 20}) CHECK_EQ(v.read(ctx, text, offset, 4096), 0);
  CHECK_EQ(mem.reads.load(), 2u);                                                                    // two blocks for four reads, no read-ahead (not sequential)
  CHECK_EQ(mem.readBytes.load(), 2u * 262144);
  // A 1 MiB read goes out as four parallel block reads (the relay carries 8 at once).
  mem.delay = std::chrono::milliseconds(15); mem.peak = 0; const size_t before = mem.reads;
  CHECK_EQ(v.read(ctx, text, 20ULL << 20, 1 << 20), 0); CHECK(text.size() == (1 << 20));
  CHECK(mem.peak >= 4); CHECK_EQ(mem.reads - before, 4u);
  // Sequential reading prefetches ahead (bounded), later reads hit the cache.
  mem.delay = std::chrono::milliseconds(0);
  const size_t sequentialStart = mem.reads;
  for (uint64_t off = 24ULL << 20; off < (26ULL << 20); off += 65536) CHECK_EQ(v.read(ctx, text, off, 65536), 0);
  const size_t fetched = mem.reads - sequentialStart;
  CHECK(fetched >= 8 && fetched <= 8 + o.prefetchBlocks + 1);                                        // 2 MiB = 8 blocks, plus at most the prefetch window
  // The cache never grows past its bound, a whole-file read of 40 MiB would be 160 blocks.
  for (uint64_t off = 0; off < (16ULL << 20); off += 262144) CHECK_EQ(v.read(ctx, text, off, 4096), 0);
  CHECK(share.cachedBlocks() <= 64);
  // A write invalidates what was cached for the file.
  mem.file("small.txt", "old data"); PVOID s = nullptr; CHECK_EQ(v.open(L"\\small.txt", &s, &info), 0);
  CHECK_EQ(v.read(s, text, 0, 8), 0); CHECK(text == "old data"); FSP_FSCTL_FILE_INFO w{}; CHECK_EQ(v.write(s, "NEW", 0, &w), 0);
  CHECK_EQ(v.read(s, text, 0, 8), 0); CHECK(text == "NEW data");
  // The PC file changed behind our back: a new open sees it (cache keyed by size and time).
  v.close(s); mem.file("small.txt", "changed behind"); mem.nodes[lowerKey("small.txt")].modified = 1761399999999;
  Sleep(1100); CHECK_EQ(v.open(L"\\small.txt", &s, &info), 0); CHECK_EQ(v.read(s, text, 0, 32), 0); CHECK(text == "changed behind"); v.close(s);
  v.close(ctx); share.unmount();
}

void officePatterns() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.file("report.docx", "v1"); PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{};
  // Word: write ~WRD0001.tmp, rename report.docx -> backup, rename the temp over report.docx.
  CHECK_EQ(v.create(L"\\~WRD0001.tmp", false, &ctx, &info), 0); CHECK_EQ(v.write(ctx, "v2 contents", 0, &info), 0); v.cleanup(ctx, L"\\~WRD0001.tmp", 0);
  CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\~WRD0001.tmp"), const_cast<PWSTR>(L"\\report.docx"), FALSE), STATUS_OBJECT_NAME_COLLISION);
  CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\~WRD0001.tmp"), const_cast<PWSTR>(L"\\report.docx"), TRUE), 0);
  v.close(ctx); CHECK(!mem.exists("~WRD0001.tmp") && mem.content("report.docx") == "v2 contents");
  // Reading after the replace shows the new bytes (no stale cache from the old file).
  std::string text; CHECK_EQ(v.open(L"\\report.docx", &ctx, &info), 0); CHECK_EQ(v.read(ctx, text, 0, 64), 0); CHECK(text == "v2 contents"); v.close(ctx);
  // Case-only rename.
  CHECK_EQ(v.open(L"\\report.docx", &ctx, &info), 0); CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\report.docx"), const_cast<PWSTR>(L"\\Report.docx"), FALSE), 0); v.close(ctx);
  CHECK(mem.nameOf("report.docx") == "Report.docx");
  // Delete on close: SetDelete checks, Cleanup removes. A non-empty folder cannot go.
  mem.dir("d"); mem.file("d/inner.txt", "x");
  CHECK_EQ(v.open(L"\\d", &ctx, &info), 0); CHECK_EQ(v.t->SetDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\d"), TRUE), STATUS_DIRECTORY_NOT_EMPTY); CHECK_EQ(v.t->CanDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\d")), STATUS_DIRECTORY_NOT_EMPTY); CHECK_EQ(v.t->SetDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\d"), FALSE), 0); v.close(ctx);
  CHECK_EQ(v.open(L"\\d\\inner.txt", &ctx, &info), 0); CHECK_EQ(v.t->SetDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\d\\inner.txt"), TRUE), 0); CHECK(mem.exists("d/inner.txt")); v.cleanup(ctx, L"\\d\\inner.txt", FspCleanupDelete); v.close(ctx); CHECK(!mem.exists("d/inner.txt"));
  CHECK_EQ(v.open(L"\\d", &ctx, &info), 0); CHECK_EQ(v.t->SetDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\d"), TRUE), 0); v.cleanup(ctx, L"\\d", FspCleanupDelete); v.close(ctx); CHECK(!mem.exists("d"));
  // Create with delete-on-close for a temp file (the whole life of the file is Create .. Cleanup(delete)).
  CHECK_EQ(v.create(L"\\temp.tmp", false, &ctx, &info), 0); CHECK_EQ(v.write(ctx, "scratch", 0, &info), 0); CHECK_EQ(v.t->SetDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\temp.tmp"), TRUE), 0); const size_t flushes = mem.flushes; v.cleanup(ctx, L"\\temp.tmp", FspCleanupDelete); v.close(ctx);
  CHECK(!mem.exists("temp.tmp") && mem.flushes == flushes);
  // Renaming a folder keeps its content reachable under the new name.
  mem.dir("old"); mem.file("old/a.txt", "A"); CHECK_EQ(v.open(L"\\old", &ctx, &info), 0); CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\old"), const_cast<PWSTR>(L"\\new"), FALSE), 0); v.close(ctx);
  CHECK(mem.exists("new/a.txt") && !mem.exists("old/a.txt"));
  CHECK_EQ(v.open(L"\\new\\a.txt", &ctx, &info), 0); v.close(ctx);
  // Rename onto an existing folder / of a missing file / of the root is refused.
  mem.dir("x1"); mem.dir("x2"); CHECK_EQ(v.open(L"\\x1", &ctx, &info), 0); CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\x1"), const_cast<PWSTR>(L"\\x2"), TRUE), STATUS_FILE_IS_A_DIRECTORY); CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\"), const_cast<PWSTR>(L"\\x3"), FALSE), STATUS_OBJECT_NAME_INVALID); v.close(ctx);
  share.unmount();
}

void directories() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.dir("pics"); const std::vector<std::string> names{"a.jpg", "B.jpg", utf8(L"\uD55C\uAE00.png"), "c d.txt", "e", "f", "g"};
  for (const auto& n : names) mem.file("pics/" + n, "1");
  mem.dir("pics/sub");
  PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{}; CHECK_EQ(v.open(L"\\pics", &ctx, &info), 0);
  bool ok = false; auto listed = v.list(ctx, &ok); CHECK(ok);
  std::set<std::wstring> got(listed.begin(), listed.end()); CHECK(got.count(L".") && got.count(L"..") && got.count(L"a.jpg") && got.count(L"B.jpg") && got.count(L"\uD55C\uAE00.png") && got.count(L"sub") && got.size() == names.size() + 3);
  CHECK_EQ(mem.count(Op::list), 3u);                                                                // 8 entries, 3 per page: three requests, no more
  // Paging through a small buffer (several ReadDirectory calls, marker driven) gives each entry once.
  auto paged = v.list(ctx, &ok, 400); CHECK(ok && std::set<std::wstring>(paged.begin(), paged.end()).size() == paged.size() && paged.size() == listed.size());
  v.close(ctx);
  // The root has no dot entries.
  CHECK_EQ(v.open(L"\\", &ctx, &info), 0); auto root = v.list(ctx, &ok); CHECK(ok && std::set<std::wstring>(root.begin(), root.end()) == std::set<std::wstring>{L"pics"}); v.close(ctx);
  // Failures surface; the listing is refreshed on a new enumeration once the cached one is older than FileInfoTimeout (the bound
  // WinFsp's own directory cache has too).
  Sleep(1100);
  CHECK_EQ(v.open(L"\\pics", &ctx, &info), 0); mem.fail["list"] = "drive_io_failed"; { auto none = v.list(ctx, &ok); CHECK(!ok); } mem.fail.clear();
  mem.file("pics/late.txt", "1"); auto fresh = v.list(ctx, &ok); CHECK(ok && std::count(fresh.begin(), fresh.end(), std::wstring(L"late.txt")) == 1); v.close(ctx);
  // 20,000+ entries are capped (the client does the same).
  Mem many; FakeFsp f2; fs::ShareFs s2(f2.api(), many, options("big")); std::wstring l2; s2.mount(l2); Vol w(f2); many.page = 256; many.dir("d");
  for (int i = 0; i < 700; ++i) many.file("d/f" + std::to_string(i), "");
  PVOID c2 = nullptr; w.open(L"\\d", &c2, &info); auto all = w.list(c2, &ok, 1 << 20); CHECK(ok && all.size() == 702); w.close(c2);
  share.unmount();
}

void readOnlyShare() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options("pics", true)); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.file("a.txt", "abc"); PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{}; std::string text;
  CHECK_EQ(v.open(L"\\a.txt", &ctx, &info), 0); CHECK((info.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0); CHECK_EQ(v.read(ctx, text, 0, 3), 0); CHECK(text == "abc");
  CHECK_EQ(v.write(ctx, "z", 0, &info), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK_EQ(v.t->SetFileSize(fsp.fs, ctx, 0, FALSE, &info), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK_EQ(v.t->Overwrite(fsp.fs, ctx, 0, FALSE, 0, &info), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK_EQ(v.t->SetDelete(fsp.fs, ctx, const_cast<PWSTR>(L"\\a.txt"), TRUE), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK_EQ(v.t->Rename(fsp.fs, ctx, const_cast<PWSTR>(L"\\a.txt"), const_cast<PWSTR>(L"\\b.txt"), FALSE), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK_EQ(v.t->SetBasicInfo(fsp.fs, ctx, 0, 0, 0, fs::toFileTime(1700000000000), 0, &info), STATUS_MEDIA_WRITE_PROTECTED);
  PVOID other = nullptr; CHECK_EQ(v.create(L"\\n.txt", false, &other, &info), STATUS_MEDIA_WRITE_PROTECTED); CHECK_EQ(v.create(L"\\nd", true, &other, &info), STATUS_MEDIA_WRITE_PROTECTED);
  CHECK(mem.count(Op::write) == 0 && mem.count(Op::create) == 0 && mem.count(Op::setsize) == 0 && mem.count(Op::rename) == 0 && mem.count(Op::remove) == 0 && mem.count(Op::touch) == 0);   // refused here, nothing reaches the relay
  v.cleanup(ctx, L"\\a.txt", 0); CHECK_EQ(mem.flushes.load(), 0u);
  // The switch in the client changes it live: writable now, read-only again later.
  share.setReadOnly(false); CHECK_EQ(v.write(ctx, "z", 0, &info), 0); CHECK(mem.content("a.txt") == "zbc");
  share.setReadOnly(true); CHECK_EQ(v.write(ctx, "y", 0, &info), STATUS_MEDIA_WRITE_PROTECTED);
  // The broker's own refusal (a stale flag) is reported the same way.
  share.setReadOnly(false); mem.fail["write"] = "drive_read_only"; CHECK_EQ(v.write(ctx, "y", 0, &info), STATUS_MEDIA_WRITE_PROTECTED);
  v.close(ctx); share.unmount();
}

void failuresAndRevocation() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); auto o = options(); o.callTimeout = std::chrono::milliseconds(150); fs::ShareFs share(api, mem, o); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.file("f.txt", "data"); PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{}; std::string text; UINT32 attr = 0;
  CHECK_EQ(v.open(L"\\f.txt", &ctx, &info), 0);
  // The PC went away (relay closed): never a hang, a network error.
  mem.fail["read"] = "drive_unavailable"; CHECK_EQ(v.read(ctx, text, 0, 4), STATUS_NETWORK_NAME_DELETED);
  mem.fail["read"] = "drive_not_found"; CHECK_EQ(v.read(ctx, text, 0, 4), STATUS_FILE_INVALID);          // the folder or file vanished mid-read
  mem.fail["read"] = "drive_io_failed"; CHECK_EQ(v.read(ctx, text, 0, 4), STATUS_UNEXPECTED_IO_ERROR);
  mem.fail["read"] = "drive_denied"; CHECK_EQ(v.read(ctx, text, 0, 4), STATUS_ACCESS_DENIED);
  mem.fail.clear(); CHECK_EQ(v.read(ctx, text, 0, 4), 0); CHECK(text == "data");                          // the failed block was dropped, a retry works
  mem.fail["write"] = "drive_unavailable"; CHECK_EQ(v.write(ctx, "x", 0, &info), STATUS_NETWORK_NAME_DELETED); mem.fail.clear();
  mem.fail["stat"] = "drive_io_failed"; share.setReadOnly(false); Sleep(1100); CHECK_EQ(v.sec(L"\\f.txt", &attr), STATUS_UNEXPECTED_IO_ERROR); mem.fail.clear(); Sleep(10); CHECK_EQ(v.sec(L"\\f.txt", &attr), 0);   // errors are not cached
  // An unanswered request ends with a timeout, not a hang.
  mem.file("g.txt", "q"); mem.hold = true; const auto start = Clock::now(); CHECK_EQ(v.sec(L"\\g.txt", &attr), STATUS_IO_TIMEOUT); CHECK(Clock::now() - start < std::chrono::seconds(3)); mem.hold = false;
  // Revocation in the middle of a read: the drive is removed first, calls in flight finish, later ones fail fast.
  mem.sparse("long.bin", 3ULL << 20); PVOID big = nullptr; CHECK_EQ(v.open(L"\\long.bin", &big, &info), 0);
  { auto o2 = options("slow"); o2.callTimeout = std::chrono::seconds(5); Mem slowMem; slowMem.sparse("x.bin", 1 << 20); slowMem.delay = std::chrono::milliseconds(200); FakeFsp f2; fs::ShareFs slow(f2.api(), slowMem, o2); std::wstring l2; CHECK_EQ(slow.mount(l2), 0); Vol w(f2);
    PVOID c = nullptr; CHECK_EQ(w.open(L"\\x.bin", &c, &info), 0);
    std::thread reader([&] { std::string t; (void)w.read(c, t, 0, 1 << 20); });
    Sleep(50); const auto begin = Clock::now(); slow.unmount(); CHECK(Clock::now() - begin < std::chrono::seconds(4));
    reader.join();   // the volume is gone: only the share object answers now, and it answers at once
    auto* file = static_cast<fs::ShareFs::File*>(c); std::string t(4, '\0'); ULONG got = 0; PVOID made = nullptr; FSP_FSCTL_VOLUME_INFO vi{};
    CHECK_EQ(slow.read(file, t.data(), 0, 4, &got), STATUS_NETWORK_NAME_DELETED); CHECK_EQ(slow.getFileInfo(file, &info), STATUS_NETWORK_NAME_DELETED);
    CHECK_EQ(slow.create(const_cast<PWSTR>(L"\\n"), 0, 0, 0, nullptr, 0, &made, &info), STATUS_NETWORK_NAME_DELETED); CHECK_EQ(slow.getVolumeInfo(&vi), STATUS_NETWORK_NAME_DELETED);
    slow.close(file); }
  v.close(big); v.close(ctx); share.unmount();
}

void cachesAndIdentity() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
  mem.file("a.txt", "1"); UINT32 attr = 0; PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.sec(L"\\a.txt", &attr), 0); CHECK_EQ(v.open(L"\\a.txt", &ctx, &info), 0); v.close(ctx); CHECK_EQ(v.sec(L"\\a.txt", &attr), 0);
  CHECK_EQ(share.statCalls.load(), 1u);                                                                 // lookup + open + lookup: one round trip inside the timeout
  CHECK_EQ(v.sec(L"\\none.txt", &attr), STATUS_OBJECT_NAME_NOT_FOUND); CHECK_EQ(v.sec(L"\\none.txt", &attr), STATUS_OBJECT_NAME_NOT_FOUND); CHECK_EQ(share.statCalls.load(), 2u);
  CHECK_EQ(v.create(L"\\none.txt", false, &ctx, &info), 0); v.close(ctx); CHECK_EQ(v.sec(L"\\none.txt", &attr), 0);   // a create clears the negative answer
  Sleep(1100); const size_t before = share.statCalls; CHECK_EQ(v.sec(L"\\a.txt", &attr), 0); CHECK_EQ(share.statCalls.load(), before + 1);   // expired
  // Unicode and long paths round trip.
  std::string deep; std::wstring wdeep; for (int i = 0; i < 6; ++i) { deep += (i ? "/" : "") + std::string(60, char('a' + i)); wdeep += L"\\" + std::wstring(60, wchar_t(L'a' + i)); mem.dir(deep); }
  mem.file(deep + "/" + utf8(L"\uD55C\uAE00 \uD30C\uC77C.txt"), "x"); CHECK_EQ(v.open((wdeep + L"\\\uD55C\uAE00 \uD30C\uC77C.txt").c_str(), &ctx, &info), 0); v.close(ctx);
  CHECK_EQ(v.sec((wdeep + L"\\" + std::wstring(256, L'z')).c_str(), &attr), STATUS_OBJECT_NAME_INVALID);
  share.unmount();
}

// The PC answers at once, but the answers of the next `holdNext[op]` requests of an op are delivered only on release(): a reply
// that was produced before something else happened and arrives after it.
class Delayed final : public Backend {
 public:
  explicit Delayed(Mem& pc) : pc_(pc) {}
  std::map<Op, size_t> holdNext;
  std::future<Result> submit(Call call) override {
    Result result = pc_.submit(call).get(); auto promise = std::make_shared<std::promise<Result>>(); auto future = promise->get_future();
    std::lock_guard lock(mutex_);
    if (auto found = holdNext.find(call.op); found != holdNext.end() && found->second) { --found->second; held_.push_back({promise, std::move(result)}); }
    else promise->set_value(std::move(result));
    return future;
  }
  size_t held() { std::lock_guard lock(mutex_); return held_.size(); }
  void release() { std::lock_guard lock(mutex_); for (auto& [promise, result] : held_) promise->set_value(std::move(result)); held_.clear(); }
 private:
  Mem& pc_; std::mutex mutex_; std::vector<std::pair<std::shared_ptr<std::promise<Result>>, Result>> held_;
};
// Listing and lookup caches: a folder view needs no lookup per entry, a listing is reused within FileInfoTimeout, a
// write through the volume is seen at once, an answer older than a mutation is never cached, and nothing cached outlives a revocation.
void listingCaches() {
  {
    Mem mem; FakeFsp fsp; const auto api = fsp.api(); auto o = options(); o.fileInfoTimeoutMs = 300; fs::ShareFs share(api, mem, o); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
    mem.page = 20; mem.dir("view"); for (int i = 0; i < 50; ++i) mem.file("view/f" + std::to_string(i) + ".txt", std::string(size_t(i + 1), 'x'));
    PVOID dir = nullptr; FSP_FSCTL_FILE_INFO info{}; bool ok = false;
    CHECK_EQ(v.open(L"\\view", &dir, &info), 0); const auto names = v.list(dir, &ok); CHECK(ok && names.size() == 52); v.close(dir);
    const size_t lists = mem.count(Op::list), stats = share.statCalls;
    for (int i = 0; i < 50; ++i) {
      const auto name = L"\\view\\f" + std::to_wstring(i) + L".txt"; UINT32 attr = 0; PVOID c = nullptr;
      CHECK_EQ(v.sec(name.c_str(), &attr), 0); CHECK_EQ(v.open(name.c_str(), &c, &info), 0); CHECK_EQ(info.FileSize, uint64_t(i + 1)); v.close(c);
    }
    CHECK_EQ(share.statCalls.load(), stats);                                                            // every lookup came from the listing
    CHECK_EQ(v.open(L"\\view", &dir, &info), 0); CHECK(v.list(dir, &ok).size() == 52 && ok); v.close(dir);
    CHECK_EQ(mem.count(Op::list), lists);                                                               // listed again at once: no request
    // A write through the volume: lookups and listings show the new size at once.
    PVOID c = nullptr; CHECK_EQ(v.open(L"\\view\\f0.txt", &c, &info), 0); FSP_FSCTL_FILE_INFO w{}; CHECK_EQ(v.write(c, "0123456789", 0, &w), 0); v.close(c);
    CHECK_EQ(v.open(L"\\view\\f0.txt", &c, &info), 0); CHECK_EQ(info.FileSize, 10u); v.close(c);
    CHECK_EQ(v.open(L"\\view", &dir, &info), 0); CHECK(v.list(dir, &ok).size() == 52 && ok); v.close(dir); CHECK(mem.count(Op::list) > lists);
    CHECK_EQ(v.open(L"\\view\\f0.txt", &c, &info), 0); CHECK_EQ(info.FileSize, 10u); v.close(c);         // the new listing carries the new size
    // A change on the PC is seen once the cached answers are older than FileInfoTimeout.
    mem.file("view/f1.txt", "changed on the PC"); Sleep(350);
    CHECK_EQ(v.open(L"\\view\\f1.txt", &c, &info), 0); CHECK_EQ(info.FileSize, 17u); v.close(c);
    // Revocation: nothing cached is served afterwards.
    CHECK_EQ(v.open(L"\\view", &dir, &info), 0); (void)v.list(dir, &ok);
    share.unmount(); UINT32 attr = 0; auto* file = static_cast<fs::ShareFs::File*>(dir); std::vector<uint8_t> buffer(4096); ULONG used = 0;
    CHECK_EQ(share.getSecurityByName(const_cast<PWSTR>(L"\\view\\f2.txt"), &attr, nullptr, nullptr), STATUS_NETWORK_NAME_DELETED);
    CHECK_EQ(share.readDirectory(file, nullptr, nullptr, buffer.data(), ULONG(buffer.size()), &used), STATUS_NETWORK_NAME_DELETED);
    share.close(file);
  }
  // A listing and a lookup answered before a write cleared the caches arrive after it: they are used, not cached.
  for (const Op op : {Op::list, Op::stat}) {
    Mem mem; Delayed link(mem); FakeFsp fsp; const auto api = fsp.api(); auto o = options(); o.fileInfoTimeoutMs = 60000; fs::ShareFs share(api, link, o); std::wstring l; CHECK_EQ(share.mount(l), 0); Vol v(fsp);
    mem.dir("d"); mem.file("d/a.txt", "1234"); PVOID dir = nullptr, c = nullptr; FSP_FSCTL_FILE_INFO info{};
    if (op == Op::list) CHECK_EQ(v.open(L"\\d", &dir, &info), 0);
    link.holdNext[op] = 1;
    std::thread early([&] { if (op == Op::list) { bool ok = false; (void)v.list(dir, &ok); } else { UINT32 attr = 0; (void)v.sec(L"\\d\\a.txt", &attr); } });
    CHECK(eventually([&] { return link.held() == 1; }));                                               // answered by the PC: a.txt has 4 bytes
    CHECK_EQ(v.open(L"\\d\\a.txt", &c, &info), 0); FSP_FSCTL_FILE_INFO w{}; CHECK_EQ(v.write(c, "123456789", 0, &w), 0); v.close(c);
    link.release(); early.join();
    UINT32 attr = 0; CHECK_EQ(v.sec(L"\\d\\a.txt", &attr), 0); CHECK_EQ(v.open(L"\\d\\a.txt", &c, &info), 0);
    CHECK_EQ(info.FileSize, 9u);                                                                       // not the 4 bytes of the late answer
    v.close(c); if (dir) v.close(dir); share.unmount();
  }
}

void openDescriptorsStayCurrent() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); auto o = options(); o.fileInfoTimeoutMs = 10;
  fs::ShareFs share(api, mem, o); std::wstring letter; CHECK_EQ(share.mount(letter), 0); Vol v(fsp);
  CHECK(fsp.params.UmFileContextIsUserContext2);
  mem.file("live.txt", "old data"); PVOID a = nullptr, b = nullptr; FSP_FSCTL_FILE_INFO info{}; std::string text;
  CHECK_EQ(v.open(L"\\live.txt", &a, &info), 0); CHECK_EQ(v.open(L"\\LIVE.txt", &b, &info), 0); CHECK(a != b);
  CHECK_EQ(v.read(a, text, 0, 8), 0); CHECK(text == "old data");
  CHECK_EQ(v.read(b, text, 0, 8), 0); CHECK(text == "old data");
  mem.file("live.txt", "new data"); { std::lock_guard lock(mem.mutex); mem.nodes["live.txt"].modified += 2000; }
  Sleep(20); CHECK_EQ(v.t->GetFileInfo(fsp.fs, a, &info), 0);
  CHECK_EQ(v.read(a, text, 0, 8), 0); CHECK(text == "new data");
  CHECK_EQ(v.read(b, text, 0, 8), 0); CHECK(text == "new data");  // refresh on read even without GetFileInfo
  mem.file("live.txt", "x"); { std::lock_guard lock(mem.mutex); mem.nodes["live.txt"].modified += 3000; }
  Sleep(20); CHECK_EQ(v.read(a, text, 1, 8), STATUS_END_OF_FILE); CHECK(text.empty());
  v.close(a); v.close(b);
  // Appends through two descriptors use the current PC size, not each Open's size.
  mem.file("append.txt", ""); CHECK_EQ(v.open(L"\\append.txt", &a, &info), 0); CHECK_EQ(v.open(L"\\append.txt", &b, &info), 0);
  CHECK_EQ(v.write(a, "one", 0, &info, true), 0); CHECK_EQ(v.write(b, "two", 0, &info, true), 0); CHECK(mem.content("append.txt") == "onetwo");
  CHECK_EQ(v.read(a, text, 3, 3), 0); CHECK(text == "two");
  v.close(a); v.close(b);
}

void openDescendantsFollowRename() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring letter; CHECK_EQ(share.mount(letter), 0); Vol v(fsp);
  mem.dir("docs"); mem.dir("docs/sub"); mem.file("docs/sub/a.txt", "hello");
  PVOID dir = nullptr, a = nullptr, b = nullptr; FSP_FSCTL_FILE_INFO info{}; std::string text;
  CHECK_EQ(v.open(L"\\docs", &dir, &info), 0); CHECK_EQ(v.open(L"\\docs\\sub\\a.txt", &a, &info), 0); CHECK_EQ(v.open(L"\\DOCS\\sub\\a.txt", &b, &info), 0);
  CHECK_EQ(v.read(a, text, 0, 8), 0);
  CHECK_EQ(v.t->Rename(fsp.fs, dir, const_cast<PWSTR>(L"\\docs"), const_cast<PWSTR>(L"\\Moved"), FALSE), 0);
  CHECK_EQ(v.read(a, text, 0, 8), 0); CHECK(text == "hello");
  CHECK_EQ(v.write(b, "H", 0, &info), 0); CHECK(mem.content("Moved/sub/a.txt") == "Hello");
  CHECK_EQ(v.t->Rename(fsp.fs, dir, const_cast<PWSTR>(L"\\Moved"), const_cast<PWSTR>(L"\\moved"), FALSE), 0);
  CHECK_EQ(v.read(b, text, 0, 8), 0); CHECK(text == "Hello");
  v.close(a); v.close(b); v.close(dir);
  // Replaced destination descriptors fail honestly; they cannot write/delete the new object.
  mem.file("old.txt", "old"); mem.file("temp.txt", "new");
  CHECK_EQ(v.open(L"\\old.txt", &a, &info), 0); CHECK_EQ(v.open(L"\\temp.txt", &b, &info), 0);
  CHECK_EQ(v.t->Rename(fsp.fs, b, const_cast<PWSTR>(L"\\temp.txt"), const_cast<PWSTR>(L"\\old.txt"), TRUE), 0);
  CHECK_EQ(v.read(a, text, 0, 8), STATUS_FILE_DELETED); CHECK_EQ(v.write(a, "bad", 0, &info), STATUS_FILE_DELETED);
  v.cleanup(a, L"\\old.txt", FspCleanupDelete); CHECK(mem.content("old.txt") == "new");
  CHECK_EQ(v.read(b, text, 0, 8), 0); CHECK(text == "new"); v.close(a); v.close(b);
}

void boundsAndVolumeFlush() {
  Mem mem; FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, mem, options()); std::wstring letter; CHECK_EQ(share.mount(letter), 0); Vol v(fsp);
  mem.file("bounded.txt", "safe"); PVOID file = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.open(L"\\bounded.txt", &file, &info), 0);
  const size_t before = mem.count(Op::write);
  CHECK_EQ(v.write(file, std::string(chunkBytes + 1, 'x'), UINT64_MAX - 3, &info), STATUS_INVALID_PARAMETER);
  CHECK_EQ(v.write(file, "x", wire::maxFileSize, &info), STATUS_INVALID_PARAMETER);
  CHECK_EQ(v.t->SetFileSize(fsp.fs, file, UINT64_MAX, TRUE, &info), STATUS_INVALID_PARAMETER);
  CHECK(mem.count(Op::write) == before && mem.content("bounded.txt") == "safe");
  Call bad; bad.op = Op::write; bad.share = "docs"; bad.path = "bounded.txt"; bad.offset = wire::maxFileSize; bad.data = {'x'};
  Call decoded; CHECK(!wire::callParse(wire::callFrame(1, bad), decoded));
  CHECK_EQ(v.write(file, "S", 0, &info), 0);
  mem.fail["flush"] = "drive_io_failed"; CHECK_EQ(v.t->Flush(fsp.fs, nullptr, &info), STATUS_UNEXPECTED_IO_ERROR);
  mem.fail.clear(); CHECK_EQ(v.t->Flush(fsp.fs, nullptr, &info), 0); CHECK_EQ(mem.flushes.load(), 1u);
  CHECK_EQ(v.t->Flush(fsp.fs, nullptr, &info), 0); CHECK_EQ(mem.flushes.load(), 1u);
  share.setReadOnly(true); const auto removes = mem.count(Op::remove); v.cleanup(file, L"\\bounded.txt", FspCleanupDelete);
  CHECK_EQ(mem.count(Op::remove), removes); CHECK(mem.exists("bounded.txt")); v.close(file);
}

// ---- pipes and processes without processes -----------------------------------------------
class Pipe {
 public:
  void write(const uint8_t* data, size_t size) { std::lock_guard lock(mutex_); if (closed_) return; bytes_.insert(bytes_.end(), data, data + size); changed_.notify_all(); }
  size_t read(uint8_t* out, size_t size) { std::unique_lock lock(mutex_); changed_.wait(lock, [&] { return closed_ || !bytes_.empty(); }); if (bytes_.empty()) return 0; const size_t n = std::min(size, bytes_.size()); for (size_t i = 0; i < n; ++i) { out[i] = bytes_.front(); bytes_.pop_front(); } return n; }
  void close() { { std::lock_guard lock(mutex_); closed_ = true; } changed_.notify_all(); }
 private:
  std::mutex mutex_; std::condition_variable changed_; std::deque<uint8_t> bytes_; bool closed_ = false;
};
struct Link2 { Pipe toHelper, toService; };

void bridgeAndHelper() {
  Mem pc; FakeFsp fsp; const auto api = fsp.api(); Link2 link; std::atomic<bool> lost{false};
  fs::Helper helper(api, {[&](uint8_t* b, size_t n) { return link.toHelper.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toService.write(bytes.data(), bytes.size()); return true; }}, currentSid());
  std::atomic<int> code{-1}; std::thread helperThread([&] { code = helper.run(); });
  fs::FsBridge bridge(pc, [&](uint8_t* b, size_t n) { return link.toService.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toHelper.write(bytes.data(), bytes.size()); return true; }, [&] { link.toHelper.close(); }, [&] { lost = true; });
  CHECK(bridge.waitReady(std::chrono::seconds(5)).empty());
  pc.dir("docs"); pc.file("docs/a.txt", "from the pc");
  const std::string token = std::string(sharePrefix) + randomToken();
  auto mounted = bridge.mount({token, "docs", "Documents (\xEB\x82\xB4 PC)", false}, std::chrono::seconds(5));
  CHECK(mounted.letter == "Z" && mounted.error.empty());
  CHECK(helper.mounted() == 1);
  Vol v(fsp);
  // A read inside the VM goes helper -> service -> relay (the PC) and back.
  PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{}; std::string text;
  CHECK_EQ(v.open(L"\\docs\\a.txt", &ctx, &info), 0); CHECK_EQ(v.read(ctx, text, 0, 64), 0); CHECK(text == "from the pc");
  CHECK_EQ(v.write(ctx, "X", 0, &info), 0); CHECK(pc.content("docs/a.txt") == "Xrom the pc"); v.cleanup(ctx, L"\\docs\\a.txt", 0); CHECK(pc.flushes >= 1); v.close(ctx);
  CHECK(bridge.served() >= 4);                                                                          // stat, read, write, flush
  const auto countOf = [&](const char* name) { const auto log = fsp.log(); return std::count(log.begin(), log.end(), std::string(name)); };
  // A second share with a failing mount (no letter) is reported and forgotten.
  fsp.mountStatus = STATUS_NO_SUCH_DEVICE;
  auto failed = bridge.mount({std::string(sharePrefix) + randomToken(), "pics", "Pictures", false}, std::chrono::seconds(5)); CHECK(failed.letter.empty() && failed.error == "drive_letter_unavailable" && failed.status == STATUS_NO_SUCH_DEVICE);
  fsp.mountStatus = STATUS_ACCESS_DENIED;
  auto denied = bridge.mount({std::string(sharePrefix) + randomToken(), "pics", "Pictures", false}, std::chrono::seconds(5)); CHECK(denied.error == "drive_map_failed");
  fsp.mountStatus = 0; CHECK(helper.mounted() == 1);
  // read-only changes reach the live volume.
  bridge.setReadOnly(token, true); CHECK(eventually([&] { PVOID c = nullptr; FSP_FSCTL_FILE_INFO i{}; if (v.open(L"\\docs\\a.txt", &c, &i)) return false; const NTSTATUS s = v.write(c, "Z", 0, &i); v.close(c); return s == STATUS_MEDIA_WRITE_PROTECTED; }));
  bridge.setReadOnly(token, false);
  // Unmounting ends access first.
  const auto unmountsBefore = countOf("unmount"), deletesBefore = countOf("delete");
  CHECK(bridge.unmount(token, std::chrono::seconds(5))); CHECK(helper.mounted() == 0);
  CHECK(countOf("unmount") == unmountsBefore + 1 && countOf("delete") == deletesBefore + 1);
  // The helper's calls for a share the service does not know are refused by the bridge.
  Call stranger; stranger.op = Op::stat; stranger.share = "other"; stranger.path = "";
  const auto frame = wire::encode(wire::callFrame(900, stranger)); link.toService.write(frame.data(), frame.size());
  // (no helper-side share named "other" exists; the reply is a result frame the helper drops - the bridge must stay up)
  CHECK(bridge.alive());
  bridge.signalStop(); helperThread.join(); CHECK_EQ(code.load(), 0);
  link.toService.close(); bridge.stop();                                                                // the helper ended: its output pipe closes
  CHECK(!lost);                                                                                         // a requested stop is not a loss
}

void concurrentAppendsKeepBothWrites() {
  Mem pc; pc.file("append.txt", ""); pc.file("read.txt", "readable"); pc.file("write.txt", "old");
  class HeldFirstWrite final : public Backend {
   public:
    explicit HeldFirstWrite(Mem& pc) : pc_(pc) {}
    std::promise<void> entered;
    std::future<Result> submit(Call call) override {
      if (call.op == Op::write && lowerKey(call.path) == "append.txt") {
        std::lock_guard lock(mutex_); offsets_.push_back(call.offset);
        if (!saved_) { saved_ = std::move(call); auto future = held_.get_future(); entered.set_value(); return future; }
      }
      return pc_.submit(std::move(call));
    }
    void release() {
      std::optional<Call> call;
      { std::lock_guard lock(mutex_); if (!saved_ || released_) return; released_ = true; call = saved_; }
      held_.set_value(pc_.submit(std::move(*call)).get());
    }
    std::vector<uint64_t> offsets() { std::lock_guard lock(mutex_); return offsets_; }
    ~HeldFirstWrite() override { release(); }
   private:
    Mem& pc_; std::mutex mutex_; std::optional<Call> saved_; std::promise<Result> held_; bool released_ = false;
    std::vector<uint64_t> offsets_;
  } backend(pc);
  FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, backend, options()); std::wstring letter; CHECK_EQ(share.mount(letter), 0); Vol v(fsp);
  PVOID first = nullptr, second = nullptr, reader = nullptr, writer = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.open(L"\\append.txt", &first, &info), 0); CHECK_EQ(v.open(L"\\APPEND.TXT", &second, &info), 0);
  CHECK_EQ(v.open(L"\\read.txt", &reader, &info), 0); CHECK_EQ(v.open(L"\\write.txt", &writer, &info), 0);
  auto entered = backend.entered.get_future();
  auto firstWrite = std::async(std::launch::async, [&] { FSP_FSCTL_FILE_INFO i{}; ULONG done = 0; const auto status = v.write(first, "A", 0, &i, true, false, &done); return std::pair{status, done}; });
  CHECK(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  std::promise<void> secondStarted; auto started = secondStarted.get_future();
  auto secondWrite = std::async(std::launch::async, [&] { secondStarted.set_value(); FSP_FSCTL_FILE_INFO i{}; ULONG done = 0; const auto status = v.write(second, "B", 0, &i, true, false, &done); return std::pair{status, done}; });
  CHECK(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  auto unrelatedRead = std::async(std::launch::async, [&] { std::string text; const auto status = v.read(reader, text, 0, 8); return std::pair{status, text}; });
  auto unrelatedWrite = std::async(std::launch::async, [&] { FSP_FSCTL_FILE_INFO i{}; return v.write(writer, "new", 0, &i); });
  const bool readReady = unrelatedRead.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool writeReady = unrelatedWrite.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  // The backend retains A without changing the PC. Before the fix B reaches
  // the same old EOF and completes here, so releasing A overwrites B. With
  // per-file serialization B remains queued; unrelated files still complete.
  const bool secondBeforeRelease = secondWrite.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
  backend.release();
  const auto a = firstWrite.get(), b = secondWrite.get(); const auto r = unrelatedRead.get(); const auto w = unrelatedWrite.get();
  const auto offsets = backend.offsets();
  CHECK_EQ(a.first, 0); CHECK_EQ(a.second, 1u); CHECK_EQ(b.first, 0); CHECK_EQ(b.second, 1u);
  CHECK(readReady && writeReady); CHECK_EQ(r.first, 0); CHECK(r.second == "readable"); CHECK_EQ(w, 0); CHECK(pc.content("write.txt") == "new");
  CHECK(!secondBeforeRelease); CHECK(pc.content("append.txt") == "AB"); CHECK(offsets == std::vector<uint64_t>({0, 1}));
  std::cout << "{\"kind\":\"two-handle-append-gated\",\"secondCompletedBeforeFirstRelease\":" << (secondBeforeRelease ? "true" : "false") << ",\"finalBytes\":" << pc.sizeOf("append.txt") << ",\"expectedBytes\":2,\"unrelatedReadCompleted\":" << (readReady ? "true" : "false") << ",\"unrelatedWriteCompleted\":" << (writeReady ? "true" : "false") << ",\"realMount\":false,\"pcExternalAppendAtomicityTested\":false}\n";
  v.close(first); v.close(second); v.close(reader); v.close(writer); share.unmount();
}

void mutationGateKeepsChunkWindowAndBoundsWait() {
  for (const bool queuedTimeout : {false, true}) {
    Mem pc; pc.file("window.bin", "");
    class Window final : public Backend {
     public:
      explicit Window(Mem& pc) : pc_(pc) {}
      std::promise<void> full; std::atomic<unsigned> writes{0};
      std::future<Result> submit(Call call) override {
        if (call.op == Op::write) {
          ++writes; std::lock_guard lock(mutex_);
          if (!released_) { auto promise = std::make_shared<std::promise<Result>>(); auto future = promise->get_future(); held_.emplace_back(std::move(call), promise); if (held_.size() == maxInFlight) full.set_value(); return future; }
        }
        return pc_.submit(std::move(call));
      }
      void release() {
        std::vector<std::pair<Call, std::shared_ptr<std::promise<Result>>>> held;
        { std::lock_guard lock(mutex_); released_ = true; held.swap(held_); }
        for (auto& [call, promise] : held) promise->set_value(pc_.submit(std::move(call)).get());
      }
      ~Window() override { release(); }
     private:
      Mem& pc_; std::mutex mutex_; bool released_ = false;
      std::vector<std::pair<Call, std::shared_ptr<std::promise<Result>>>> held_;
    } backend(pc);
    auto o = options(); o.callTimeout = std::chrono::milliseconds(queuedTimeout ? 500 : 3000);
    FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, backend, o); std::wstring letter; CHECK_EQ(share.mount(letter), 0); Vol v(fsp);
    PVOID first = nullptr, second = nullptr; FSP_FSCTL_FILE_INFO info{}; CHECK_EQ(v.open(L"\\window.bin", &first, &info), 0); CHECK_EQ(v.open(L"\\WINDOW.BIN", &second, &info), 0);
    const std::string bytes(size_t(chunkBytes) * maxInFlight, 'X'); auto full = backend.full.get_future();
    auto writer = std::async(std::launch::async, [&] { FSP_FSCTL_FILE_INFO i{}; ULONG done = 0; const auto status = v.write(first, bytes, 0, &i, true, false, &done); return std::pair{status, done}; });
    CHECK(full.wait_for(std::chrono::seconds(2)) == std::future_status::ready); CHECK_EQ(backend.writes.load(), unsigned(maxInFlight));
    if (queuedTimeout) {
      const auto began = Clock::now();
      auto queued = std::async(std::launch::async, [&] { FSP_FSCTL_FILE_INFO i{}; ULONG done = 0; const auto status = v.write(second, "Y", 0, &i, true, false, &done); return std::pair{status, done}; });
      CHECK(queued.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
      const auto result = queued.get(); CHECK_EQ(result.first, STATUS_IO_TIMEOUT); CHECK_EQ(result.second, 0u);
      CHECK(Clock::now() - began < std::chrono::seconds(2)); CHECK_EQ(backend.writes.load(), unsigned(maxInFlight));
    }
    backend.release(); const auto result = writer.get();
    if (!queuedTimeout) { CHECK_EQ(result.first, 0); CHECK_EQ(result.second, ULONG(bytes.size())); CHECK(pc.content("window.bin") == bytes); }
    v.close(first); v.close(second); share.unmount();
  }
}

void flushDoesNotForgetConcurrentWrite() {
  Mem pc; pc.file("a.txt", "abc");
  struct FlushGate final : Backend {
    Mem& pc; std::atomic<bool> armed{true}; std::promise<void> entered; std::promise<Result> held; Call saved;
    explicit FlushGate(Mem& source) : pc(source) {}
    std::future<Result> submit(Call call) override {
      if (call.op == Op::flush && armed.exchange(false)) { saved = std::move(call); auto future = held.get_future(); entered.set_value(); return future; }
      return pc.submit(std::move(call));
    }
    void release() { held.set_value(pc.submit(saved).get()); }
  } backend(pc);
  FakeFsp fsp; const auto api = fsp.api(); fs::ShareFs share(api, backend, options()); std::wstring letter;
  CHECK_EQ(share.mount(letter), 0); Vol v(fsp); PVOID ctx = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.open(L"\\a.txt", &ctx, &info), 0); CHECK_EQ(v.write(ctx, "X", 0, &info), 0);
  auto entered = backend.entered.get_future();
  auto pending = std::async(std::launch::async, [&] { FSP_FSCTL_FILE_INFO flushed{}; return v.t->Flush(fsp.fs, ctx, &flushed); });
  CHECK(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  // The first flush is pending while a later write has already completed.
  CHECK_EQ(v.write(ctx, "Y", 1, &info), 0); backend.release(); CHECK_EQ(pending.get(), 0);
  CHECK_EQ(pc.flushes.load(), 1u); CHECK(pc.content("a.txt") == "XYc");
  v.cleanup(ctx, L"\\a.txt", 0); CHECK_EQ(pc.flushes.load(), 2u);
  v.close(ctx); share.unmount();
}

void lateMountIsCancelled() {
  Mem pc; Link2 link; FakeFsp fsp;
  std::mutex gate; std::condition_variable wake; bool entered = false, released = false;
  fsp.beforeMount = [&] { std::unique_lock lock(gate); entered = true; wake.notify_all(); wake.wait(lock, [&] { return released; }); };
  const auto api = fsp.api();
  fs::Helper helper(api, {[&](uint8_t* b, size_t n) { return link.toHelper.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toService.write(bytes.data(), bytes.size()); return true; }}, currentSid());
  std::atomic<int> code{-1}; std::thread helperThread([&] { code = helper.run(); });
  fs::FsBridge bridge(pc, [&](uint8_t* b, size_t n) { return link.toService.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toHelper.write(bytes.data(), bytes.size()); return true; }, [&] { link.toHelper.close(); });
  CHECK(bridge.waitReady(std::chrono::seconds(2)).empty());
  const std::string token = std::string(sharePrefix) + randomToken();
  auto pending = std::async(std::launch::async, [&] { return bridge.mount({token, "docs", "Documents", false}, std::chrono::milliseconds(100)); });
  { std::unique_lock lock(gate); CHECK(wake.wait_for(lock, std::chrono::seconds(2), [&] { return entered; })); }
  const auto result = pending.get(); CHECK(result.letter.empty() && result.error == "drive_map_failed");
  { std::lock_guard lock(gate); released = true; } wake.notify_all();
  // The mount really completes after its caller timed out. It must be removed
  // while the helper stays alive; helper shutdown must not hide an orphan.
  CHECK(eventually([&] { const auto events = fsp.log(); return std::count(events.begin(), events.end(), "unmount") == 1 && helper.mounted() == 0; }));
  CHECK(bridge.alive());
  const std::string replacement = std::string(sharePrefix) + randomToken();
  const auto next = bridge.mount({replacement, "docs", "Documents", false}, std::chrono::seconds(2));
  CHECK(next.error.empty() && next.letter == "Z" && helper.mounted() == 1);
  CHECK(bridge.unmount(replacement, std::chrono::seconds(2))); CHECK(helper.mounted() == 0);
  bridge.signalStop(); helperThread.join(); CHECK_EQ(code.load(), 0); link.toService.close(); bridge.stop();
}

void helperLossAndMissingWinFsp() {
  // The helper dies: the service learns, pending calls fail instead of hanging.
  { Mem pc; Link2 link; std::atomic<bool> lost{false};
    fs::FsBridge bridge(pc, [&](uint8_t* b, size_t n) { return link.toService.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toHelper.write(bytes.data(), bytes.size()); return true; }, [&] { link.toHelper.close(); }, [&] { lost = true; });
    const auto ready = wire::encode(wire::controlFrame("{\"type\":\"ready\"}")); link.toService.write(ready.data(), ready.size()); CHECK(bridge.waitReady(std::chrono::seconds(2)).empty());
    link.toService.close(); CHECK(eventually([&] { return !bridge.alive() && lost.load(); }));
    auto r = bridge.mount({std::string(sharePrefix) + randomToken(), "docs", "D", false}, std::chrono::milliseconds(300)); CHECK(r.letter.empty() && r.error == "drive_map_failed");
    bridge.stop(); }
  // WinFsp missing in the user session: the helper says so, the service reports a fixed error.
  { Mem pc; Link2 link; fs::Api none; std::atomic<int> code{-1};
    fs::Helper helper(none, {[&](uint8_t* b, size_t n) { return link.toHelper.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toService.write(bytes.data(), bytes.size()); return true; }}, currentSid());
    std::thread t([&] { code = helper.run(); });
    fs::FsBridge bridge(pc, [&](uint8_t* b, size_t n) { return link.toService.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toHelper.write(bytes.data(), bytes.size()); return true; }, [&] { link.toHelper.close(); });
    CHECK(bridge.waitReady(std::chrono::seconds(5)) == "winfsp_missing"); t.join(); CHECK_EQ(code.load(), 3); link.toService.close(); bridge.stop(); }
  // A service that stops answering: pending calls fail when the pipe closes.
  { Link2 link; fs::PipeBackend backend([&](const std::vector<uint8_t>& bytes) { link.toService.write(bytes.data(), bytes.size()); return true; });
    Call c; c.op = Op::stat; c.share = "docs"; auto f = backend.submit(c); CHECK(backend.pending() == 1); backend.close(); CHECK(f.get().error == "drive_unavailable"); auto g = backend.submit(c); CHECK(g.get().error == "drive_unavailable"); }
  { fs::PipeBackend backend([](const std::vector<uint8_t>&) { return false; }); Call c; c.op = Op::stat; c.share = "docs"; CHECK(backend.submit(c).get().error == "drive_unavailable" && backend.pending() == 0); }
}

struct SimulatedProcessState {
  Pipe output; std::atomic<bool> ended{false}; std::atomic<unsigned> terminated{0};
};
class SimulatedProcess final : public fs::UserProcess {
 public:
  explicit SimulatedProcess(std::shared_ptr<SimulatedProcessState> state, const std::string& mode) : state_(std::move(state)) {
    if (mode == "unavailable") reply("{\"type\":\"unavailable\",\"error\":\"winfsp_missing\"}");
    if (mode != "ready") closeInput(); else reply("{\"type\":\"ready\"}");
  }
  size_t read(uint8_t* out, size_t size) override { return state_->output.read(out, size); }
  bool write(const std::vector<uint8_t>& bytes) override {
    if (state_->ended || bytes.size() < 13) return false;
    wire::Frame frame; if (!wire::decode(bytes.data() + 4, bytes.size() - 4, frame)) return false;
    Json json = JsonParser::parse(frame.header); const auto type = json.get("type").string();
    if (type == "mount") reply("{\"type\":\"mounted\",\"token\":" + jsonQuote(json.get("token").string()) + ",\"letter\":\"Z\"}");
    else if (type == "unmount") reply("{\"type\":\"unmounted\",\"token\":" + jsonQuote(json.get("token").string()) + "}");
    else if (type == "stop") closeInput();
    return true;
  }
  void closeInput() override { state_->ended = true; state_->output.close(); }
  bool wait(unsigned) override { return eventually([&] { return state_->ended.load(); }, 1000); }
  void terminate() override { ++state_->terminated; closeInput(); }
 private:
  std::shared_ptr<SimulatedProcessState> state_;
  void reply(const std::string& text) { const auto bytes = wire::encode(wire::controlFrame(text)); state_->output.write(bytes.data(), bytes.size()); }
};

void mounterStartupAndRecovery() {
  Mem pc; const fs::FsShare requested{std::string(sharePrefix) + randomToken(), "docs", "Documents", false};
  for (const std::string mode : {"unavailable", "eof"}) {
    auto process = std::make_shared<SimulatedProcessState>(); std::atomic<unsigned> lost{0};
    fs::WinFspMounter mounter([&](DWORD) { return std::make_shared<SimulatedProcess>(process, mode); }, std::chrono::milliseconds(100));
    mounter.onLost([&] { ++lost; mounter.onLost({}); });  // callback may acquire mounter state
    std::promise<std::vector<fs::FsMountResult>> complete; auto result = complete.get_future();
    std::thread startup([&] { complete.set_value(mounter.map(149, pc, {requested})); });
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
      std::cerr << "FAIL: early helper exit deadlocked startup/fallback\n"; std::exit(1);
    }
    const auto outcomes = result.get(); startup.join();
    CHECK(outcomes.size() == 1 && outcomes[0].letter.empty() && outcomes[0].error == "drive_helper_failed");
    CHECK(lost == 0); CHECK(process->ended); mounter.stop();
  }
  std::vector<std::shared_ptr<SimulatedProcessState>> processes; std::vector<DWORD> sessions; std::atomic<unsigned> lost{0};
  fs::WinFspMounter mounter([&](DWORD session) {
    sessions.push_back(session); auto process = std::make_shared<SimulatedProcessState>(); processes.push_back(process);
    return std::make_shared<SimulatedProcess>(process, "ready");
  }, std::chrono::milliseconds(500));
  mounter.onLost([&] { ++lost; mounter.onLost({}); });
  CHECK(mounter.map(149, pc, {requested})[0].letter == "Z");
  CHECK(mounter.map(149, pc, {requested})[0].letter == "Z" && sessions.size() == 1);
  CHECK(mounter.map(150, pc, {requested})[0].letter == "Z" && sessions == std::vector<DWORD>({149, 150}));
  CHECK(processes[0]->ended); CHECK(lost == 0);  // replacing a session is intentional, never a channel loss
  processes.back()->output.close(); CHECK(eventually([&] { return lost == 1; }));
  CHECK(mounter.map(150, pc, {requested})[0].letter == "Z" && sessions.size() == 3);
  mounter.stop(); CHECK(processes.back()->ended);
}

void helperFramesAreSerialized() {
  Mem pc; pc.file("a.txt", "data"); FakeFsp fsp; const auto api = fsp.api(); Link2 link;
  std::atomic<unsigned> writers{0}, peak{0}; std::atomic<bool> mountedWrite{false};
  fs::Helper helper(api, {[&](uint8_t* b, size_t n) { return link.toHelper.read(b, n); }, [&](const std::vector<uint8_t>& bytes) {
    const unsigned active = ++writers; unsigned old = peak; while (active > old && !peak.compare_exchange_weak(old, active)) {}
    wire::Frame frame; const bool parsed = wire::decode(bytes.data() + 4, bytes.size() - 4, frame);
    if (parsed && frame.kind == wire::Kind::control && JsonParser::parse(frame.header).get("type").string() == "mounted") { mountedWrite = true; Sleep(60); }
    link.toService.write(bytes.data(), bytes.size()); --writers; return true;
  }}, currentSid());
  std::thread helperThread([&] { helper.run(); });
  fs::FsBridge bridge(pc, [&](uint8_t* b, size_t n) { return link.toService.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { link.toHelper.write(bytes.data(), bytes.size()); return true; }, [&] { link.toHelper.close(); });
  CHECK(bridge.waitReady(std::chrono::seconds(2)).empty());
  std::thread mount([&] { bridge.mount({std::string(sharePrefix) + randomToken(), "docs", "Documents", false}, std::chrono::seconds(2)); });
  CHECK(eventually([&] { return mountedWrite.load(); }));
  Vol v(fsp); PVOID file = nullptr; FSP_FSCTL_FILE_INFO info{};
  CHECK_EQ(v.open(L"\\a.txt", &file, &info), 0); v.close(file); mount.join(); CHECK_EQ(peak.load(), 1u);
  bridge.signalStop(); helperThread.join(); link.toService.close(); bridge.stop();
}

void helperQueuesAreBounded() {
  fs::PipeBackend pipe([](const std::vector<uint8_t>&) { return true; }); Call c; c.op = Op::stat; c.share = "docs";
  std::vector<std::future<Result>> pending;
  for (size_t i = 0; i < wire::maxQueuedCalls; ++i) pending.push_back(pipe.submit(c));
  CHECK_EQ(pipe.pending(), wire::maxQueuedCalls); CHECK(pipe.submit(c).get().error == "drive_busy");
  pipe.close(); for (auto& future : pending) CHECK(future.get().error == "drive_unavailable");
  fs::Workers workers(0); for (size_t i = 0; i < wire::maxQueuedCalls; ++i) CHECK(workers.post([] {}));
  CHECK(!workers.post([] {})); workers.stop();
}

// ---- the orchestrator with a fake WinFsp mounter ------------------------------------------
class FakeLink final : public Link {
 public:
  std::mutex mutex; std::condition_variable changed; std::vector<std::string> texts; bool aborted = false, started = false; Channel::Text text; Channel::Binary binary; Channel::Closed closed;
  void start(const Credentials&, const std::string&, const std::string&, Channel::Text t, Channel::Binary b, Channel::Closed c) override { text = std::move(t); binary = std::move(b); closed = std::move(c); started = true; }
  bool sendText(std::string value) override { std::lock_guard lock(mutex); if (aborted) return false; texts.push_back(std::move(value)); changed.notify_all(); return true; }
  bool sendBinary(std::vector<uint8_t>) override { return !aborted; }
  void abort() override { { std::lock_guard lock(mutex); aborted = true; } changed.notify_all(); }
  void stop() override { abort(); if (closed) closed(); }
  std::optional<std::string> wait(const std::function<bool(const std::string&)>& match, int ms = 5000) { std::unique_lock lock(mutex); std::optional<std::string> found; changed.wait_for(lock, std::chrono::milliseconds(ms), [&] { for (const auto& item : texts) if (match(item)) { found = item; return true; } return false; }); return found; }
};
class FakeMounter final : public Mounter {
 public:
  std::mutex mutex; std::vector<std::vector<std::string>> unmapped; std::vector<std::vector<std::pair<std::string, std::string>>> mapped; int prepared = 0, swept = 0; std::string prepareError; char next = 'R';
  std::string prepare() override { std::lock_guard lock(mutex); ++prepared; return prepareError; }
  void sweep(DWORD) override { std::lock_guard lock(mutex); ++swept; }
  std::vector<MountResult> map(DWORD, uint16_t, const std::vector<std::pair<std::string, std::string>>& shares) override { std::lock_guard lock(mutex); mapped.push_back(shares); std::vector<MountResult> out; for (const auto& [token, label] : shares) { (void)label; out.push_back(MountResult{token, std::string(1, next--), {}}); } return out; }
  void unmap(DWORD, const std::vector<std::string>& tokens) override { std::lock_guard lock(mutex); unmapped.push_back(tokens); }
};
class FakeFs final : public fs::FsMounter {
 public:
  std::mutex mutex; bool usable = true; std::vector<std::vector<fs::FsShare>> mapped; std::vector<std::vector<std::string>> unmapped; std::vector<std::pair<std::string, bool>> readOnly; int stops = 0; Backend* backend = nullptr; char next = 'Z'; std::function<void()> lost;
  bool available() override { return usable; }
  std::vector<fs::FsMountResult> map(DWORD, Backend& b, const std::vector<fs::FsShare>& shares) override {
    std::lock_guard lock(mutex); backend = &b; mapped.push_back(shares); std::vector<fs::FsMountResult> out;
    for (const auto& s : shares) { fs::FsMountResult r; r.token = s.token; if (s.label == "NoLetter") { r.error = "drive_letter_unavailable"; r.status = uint32_t(STATUS_NO_SUCH_DEVICE); } else if (s.label == "Broken") { r.error = "drive_map_failed"; r.status = uint32_t(STATUS_ACCESS_DENIED); } else r.letter = std::string(1, next--); out.push_back(r); }
    return out; }
  void setReadOnly(const std::string& token, bool value) override { std::lock_guard lock(mutex); readOnly.emplace_back(token, value); }
  void unmap(const std::vector<std::string>& tokens) override { std::lock_guard lock(mutex); unmapped.push_back(tokens); }
  void stop() override { std::lock_guard lock(mutex); ++stops; }
  void onLost(std::function<void()> f) override { std::lock_guard lock(mutex); lost = std::move(f); }
};
const char* readyJson(const std::string& session) { static std::string text; text = "{\"type\":\"drive.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"drive-v1\",\"limits\":{\"maxShares\":8,\"chunkBytes\":262144,\"maxInFlight\":8,\"listPage\":256,\"maxPath\":1024,\"maxName\":255,\"maxDepth\":64}}"; return text.c_str(); }

void orchestrator() {
  Credentials credentials; credentials.origin = L"https://broker.example"; credentials.token = std::string(43, 'a');
  const std::string session = "11111111-2222-3333-4444-555555555555", url = "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive";
  // 1. driveFs granted: WinFsp serves the shares; the one that cannot be mounted falls back to WebDAV.
  {
    auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>(); auto fsOwned = std::make_unique<FakeFs>();
    auto& link = *linkOwned; auto& mounter = *mounterOwned; auto& fsm = *fsOwned;
    Redirection redirection(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000), std::move(fsOwned));
    redirection.start(credentials, session, url, 1, true);
    link.text(readyJson(session));
    link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false},{\"id\":\"pics\",\"name\":\"Pictures\",\"readOnly\":true},{\"id\":\"broken\",\"name\":\"Broken\",\"readOnly\":false},{\"id\":\"full\",\"name\":\"NoLetter\",\"readOnly\":false}]}");
    const auto mounted = link.wait([](const std::string& t) { return t.find("\"state\":\"mapped\"") != std::string::npos && t.find("\"pending\"") == std::string::npos; });
    CHECK(mounted.has_value());
    if (mounted) CHECK(mounted->find("{\"id\":\"docs\",\"state\":\"mapped\",\"letter\":\"Z\"}") != std::string::npos && mounted->find("{\"id\":\"pics\",\"state\":\"mapped\",\"letter\":\"Y\"}") != std::string::npos && mounted->find("{\"id\":\"broken\",\"state\":\"mapped\",\"letter\":\"R\"}") != std::string::npos && mounted->find("{\"id\":\"full\",\"state\":\"mapped\",\"letter\":\"Q\"}") != std::string::npos);
    { std::lock_guard lock(fsm.mutex); CHECK(fsm.mapped.size() == 1 && fsm.mapped[0].size() == 4 && fsm.backend != nullptr);
      std::map<std::string, fs::FsShare> byId; for (const auto& s : fsm.mapped[0]) byId[s.id] = s;
      CHECK(byId["pics"].readOnly && !byId["docs"].readOnly && shareToken(byId["docs"].token) && byId["docs"].label == "Documents" && byId["docs"].token != byId["pics"].token); }
    { std::lock_guard lock(mounter.mutex); CHECK(mounter.prepared == 1 && mounter.mapped.size() == 1 && mounter.mapped[0].size() == 2); }   // only the two that WinFsp could not mount; WebClient touched once
    // Read-only switch: the live WinFsp volume is told; the WebDAV one is not.
    link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":true},{\"id\":\"pics\",\"name\":\"Pictures\",\"readOnly\":true},{\"id\":\"broken\",\"name\":\"Broken\",\"readOnly\":true},{\"id\":\"full\",\"name\":\"NoLetter\",\"readOnly\":false}]}");
    CHECK(eventually([&] { std::lock_guard lock(fsm.mutex); return fsm.readOnly.size() == 1; }));
    { std::lock_guard lock(fsm.mutex); CHECK(fsm.readOnly[0].first == redirection.token("docs") && fsm.readOnly[0].second); }
    // Withdrawn: the right mounter unmaps it.
    const auto docsToken = redirection.token("docs"), brokenToken = redirection.token("broken");
    link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"pics\",\"name\":\"Pictures\",\"readOnly\":true},{\"id\":\"full\",\"name\":\"NoLetter\",\"readOnly\":false}]}");
    CHECK(eventually([&] { std::lock_guard lock(fsm.mutex); std::lock_guard lock2(mounter.mutex); return fsm.unmapped.size() == 1 && mounter.unmapped.size() == 1; }));
    { std::lock_guard lock(fsm.mutex); CHECK(fsm.unmapped[0] == std::vector<std::string>{docsToken}); }
    { std::lock_guard lock(mounter.mutex); CHECK(mounter.unmapped[0] == std::vector<std::string>{brokenToken}); }
    // The channel ends: everything left is unmapped by its own mounter and the helper is stopped.
    link.closed();
    CHECK(eventually([&] { std::lock_guard lock(fsm.mutex); return fsm.stops == 1 && fsm.unmapped.size() == 2; }));
    { std::lock_guard lock(fsm.mutex); CHECK(fsm.unmapped[1] == std::vector<std::string>{redirection.token("pics")}); }
    redirection.stop();
  }
  // 2. A WinFsp-only success never starts the WebClient service.
  {
    auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>(); mounterOwned->prepareError = "drive_webclient_unavailable"; auto fsOwned = std::make_unique<FakeFs>();
    auto& link = *linkOwned; auto& mounter = *mounterOwned;
    Redirection redirection(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000), std::move(fsOwned));
    redirection.start(credentials, session, url, 1, true); link.text(readyJson(session));
    link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false}]}");
    CHECK(link.wait([](const std::string& t) { return t.find("{\"id\":\"docs\",\"state\":\"mapped\",\"letter\":\"Z\"}") != std::string::npos; }).has_value());
    { std::lock_guard lock(mounter.mutex); CHECK(mounter.prepared == 0 && mounter.swept == 0 && mounter.mapped.empty()); }
    redirection.stop();
  }
  // 3. WinFsp not usable (or not granted): the unchanged WebDAV path, WinFsp untouched.
  for (const bool granted : {true, false}) {
    auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>(); auto fsOwned = std::make_unique<FakeFs>(); fsOwned->usable = false;
    auto& link = *linkOwned; auto& mounter = *mounterOwned; auto& fsm = *fsOwned;
    Redirection redirection(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000), std::move(fsOwned));
    redirection.start(credentials, session, url, 1, granted); link.text(readyJson(session));
    link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false}]}");
    CHECK(link.wait([](const std::string& t) { return t.find("{\"id\":\"docs\",\"state\":\"mapped\",\"letter\":\"R\"}") != std::string::npos; }).has_value());
    { std::lock_guard lock(mounter.mutex); CHECK(mounter.prepared == 1 && mounter.swept == 1 && mounter.mapped.size() == 1); }
    { std::lock_guard lock(fsm.mutex); CHECK(fsm.mapped.empty()); }
    redirection.stop();
  }
  // 4. Granted and usable but the session did not negotiate driveFs: WinFsp is never used.
  {
    auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>(); auto fsOwned = std::make_unique<FakeFs>();
    auto& link = *linkOwned; auto& fsm = *fsOwned;
    Redirection redirection(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000), std::move(fsOwned));
    redirection.start(credentials, session, url, 1, false); link.text(readyJson(session));
    link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false}]}");
    CHECK(link.wait([](const std::string& t) { return t.find("\"state\":\"mapped\"") != std::string::npos; }).has_value());
    { std::lock_guard lock(fsm.mutex); CHECK(fsm.mapped.empty()); }
    redirection.stop();
  }
  // 5. The helper ending on its own ends the drive channel (the stream continues, the user reconnects to share again).
  {
    auto linkOwned = std::make_unique<FakeLink>(); auto fsOwned = std::make_unique<FakeFs>(); auto& link = *linkOwned; auto& fsm = *fsOwned;
    Redirection redirection(std::move(linkOwned), std::make_unique<FakeMounter>(), std::chrono::milliseconds(3000), std::move(fsOwned));
    redirection.start(credentials, session, url, 1, true); link.text(readyJson(session));
    std::function<void()> lost; { std::lock_guard lock(fsm.mutex); lost = fsm.lost; } CHECK(static_cast<bool>(lost));
    if (lost) lost(); CHECK(link.aborted);
    redirection.stop();
  }
}

// ---- installing WinFsp (setup_winfsp.hpp) ----------------------------------------------------
void installer() {
  using namespace multitap::winfsp;
  CHECK(parseVersion("2.1.25156") == std::optional<Version>(Version{2, 1, 25156}));
  CHECK(parseVersion("2.1.25156.0") == std::optional<Version>(Version{2, 1, 25156}));
  for (const char* bad : {"", "2", "2.1", "2.1.x", "2.1.25156.7", "2.1.25156.", ".2.1.1", "2..1", "99999999999.1.1", "2.1.25156 beta"}) CHECK(!parseVersion(bad));
  CHECK(*parseVersion("2.0.99999") < *parseVersion("2.1.0")); CHECK(*parseVersion("2.1.25155") < *parseVersion("2.1.25156")); CHECK(!(*parseVersion("2.1.25156") < *parseVersion("2.1.25156")));
  const auto packaged = *parseVersion(PackagedVersion);
  CHECK(stateOf(std::nullopt, false, packaged) == State::none);
  CHECK(stateOf(parseVersion("2.0.1"), true, packaged) == State::older);
  CHECK(stateOf(parseVersion("2.1.25156"), true, packaged) == State::current);
  CHECK(stateOf(parseVersion("2.2.26000"), true, packaged) == State::current);                    // never a downgrade
  CHECK(stateOf(std::nullopt, true, packaged) == State::current);                                 // present but unreadable: not touched
  const std::wstring msi = L"C:\\Program Files\\Multitap\\Host\\0.2.70\\bin\\winfsp\\winfsp-2.1.25156.msi";
  struct Case { const char* mode; const char* error; const char* action; bool restart; size_t installs, owned, verifies; };
  const std::vector<Case> cases{
    {"", "", "installed", false, 1, 1, 1}, {"old", "", "upgraded", false, 1, 0, 1}, {"current", "", "skipped", false, 0, 0, 1}, {"newer", "", "skipped", false, 0, 0, 1}, {"unreadable", "", "skipped", false, 0, 0, 1},
    {"reboot", "", "installed", true, 1, 1, 1}, {"install-fails", "winfsp_install_failed", "", false, 1, 0, 0}, {"busy", "winfsp_busy", "", false, 1, 0, 0}, {"verify-fails", "winfsp_load_failed", "", false, 1, 0, 1}, {"package-bad", "winfsp_package_mismatch", "", false, 0, 0, 0}};
  for (const auto& c : cases) {
    Fake fake(c.mode); const auto out = ensureInstalled(fake, msi);
    CHECK(out.error == c.error); if (!*c.error) { CHECK(std::string(out.action) == c.action && out.restart == c.restart); }
    CHECK(fake.installs == c.installs && fake.owned == c.owned && fake.verifies == c.verifies);
    if (std::string(c.mode) == "package-bad") CHECK(fake.hashes == 1);
    if (std::string(c.mode) == "current") CHECK(fake.hashes == 0);                                // an installed WinFsp is never re-verified against our MSI
  }
  { Fake current("current"); current.installedNow = false; const auto out = ensureInstalled(current, msi); CHECK(out.error.empty()); }
  // The pins equal the provenance file (and so the file the build script bundles).
#ifdef MULTITAP_WINFSP_PIN
  { std::ifstream file(MULTITAP_WINFSP_PIN, std::ios::binary); std::string text((std::istreambuf_iterator<char>(file)), {}); CHECK(!text.empty());
    CHECK(text.find(std::string("\"sha256\":  \"") + MsiSha256 + "\"") != std::string::npos || text.find(std::string("\"sha256\": \"") + MsiSha256 + "\"") != std::string::npos);
    CHECK(text.find(std::string("\"name\":  \"") + multitap::payload::utf8(MsiName) + "\"") != std::string::npos || text.find(std::string("\"name\": \"") + multitap::payload::utf8(MsiName) + "\"") != std::string::npos);
    CHECK(text.find(std::string("\"version\":  \"") + PackagedVersion + "\"") != std::string::npos || text.find(std::string("\"version\": \"") + PackagedVersion + "\"") != std::string::npos);
    CHECK(text.find("\"size\":  " + std::to_string(MsiSize)) != std::string::npos || text.find("\"size\": " + std::to_string(MsiSize)) != std::string::npos);
    CHECK(text.find("{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}") != std::string::npos); }
#endif
  CHECK(std::wstring(UpgradeCode) == L"{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}");
}

void realWinFsp() {
  // This machine is a development PC: report, never require. The service's install check must not throw either way.
  const bool installed = fs::winfspInstalled();
  std::cout << "winfsp installed on this machine: " << (installed ? "yes" : "no") << "\n";
  const auto library = fs::winfspLibraryPath(); (void)library;
}
}

int main() {
  WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data);
  const std::vector<std::pair<const char*, void (*)()>> sections{{"rules", rules}, {"wire", wireChecks}, {"mount", mountAndSecurity}, {"security", securityDescriptor}, {"basic", basicFlow}, {"sizes", sizesAndTruncation}, {"readahead", readAhead}, {"office", officePatterns}, {"directories", directories}, {"readonly", readOnlyShare}, {"failures", failuresAndRevocation}, {"caches", cachesAndIdentity}, {"listing-caches", listingCaches}, {"descriptors", openDescriptorsStayCurrent}, {"open-rename", openDescendantsFollowRename}, {"bounds-flush", boundsAndVolumeFlush}, {"bridge", bridgeAndHelper}, {"append-race", concurrentAppendsKeepBothWrites}, {"mutation-window", mutationGateKeepsChunkWindowAndBoundsWait}, {"flush-write-race", flushDoesNotForgetConcurrentWrite}, {"late-mount", lateMountIsCancelled}, {"helper", helperLossAndMissingWinFsp}, {"startup-recovery", mounterStartupAndRecovery}, {"frame-serialization", helperFramesAreSerialized}, {"queue-bounds", helperQueuesAreBounded}, {"orchestrator", orchestrator}, {"installer", installer}, {"real", realWinFsp}};
  try { for (const auto& [name, run] : sections) { try { std::cerr << "[" << name << "]\n"; run(); } catch (const std::exception& error) { ++failures; std::cerr << "FAIL exception in " << name << ": " << error.what() << "\n"; } } }
  catch (const std::exception& error) { ++failures; std::cerr << "FAIL exception: " << error.what() << "\n"; }
  std::cout << "winfsp drive backend checks: " << checks << ", failures: " << failures << "\n";
  WSACleanup();
  return failures ? 1 : 0;
}
