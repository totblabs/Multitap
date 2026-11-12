// drive-v1 Host checks (docs/drive-redirection.md): the shared name/path
// rules, the loopback WebDAV endpoint against an in-memory client file system
// over real loopback sockets, the request multiplexer's exact wire format and
// bounds, the drive-letter helpers' pure parts, and the session orchestrator
// with a fake relay and a fake mounter (mapping, revocation, end of session).
#include "drive_redirection.hpp"
#include <iostream>

using namespace host;
using namespace host::drive;

namespace {
int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; } } while (0)

// ---- an in-memory client file system ----
class MemoryBackend final : public Backend {
 public:
  struct Node { bool dir = false; std::vector<uint8_t> data; int64_t modified = 1761300000000; };
  std::mutex mutex; std::map<std::string, std::map<std::string, Node>> shares; std::vector<Call> calls;
  std::string unavailable; std::chrono::milliseconds delay{0}; uint32_t page = 2; std::map<std::string, uint64_t> bigSizes;  // a stat reports this size instead (files over 4 GiB cannot be held in memory)
  bool fold = false;  // NTFS-like: a stat finds a name in any capitalisation
  MemoryBackend() { for (const auto* share : {"docs", "pics"}) shares[share][""] = Node{true}; }
  void file(const std::string& share, const std::string& path, std::string content) { std::lock_guard lock(mutex); shares[share][path] = Node{false, std::vector<uint8_t>(content.begin(), content.end())}; }
  void dir(const std::string& share, const std::string& path) { std::lock_guard lock(mutex); shares[share][path] = Node{true}; }
  std::string content(const std::string& share, const std::string& path) { std::lock_guard lock(mutex); const auto& node = shares[share][path]; return std::string(node.data.begin(), node.data.end()); }
  bool exists(const std::string& share, const std::string& path) { std::lock_guard lock(mutex); return shares[share].count(path) != 0; }
  std::future<Result> submit(Call call) override {
    if (delay.count()) { auto future = std::async(std::launch::async, [this, call] { std::this_thread::sleep_for(delay); return run(call); }); return future; }
    std::promise<Result> promise; promise.set_value(run(call)); return promise.get_future();
  }
 private:
  static std::string parent(const std::string& path) { const auto slash = path.rfind('/'); return slash == std::string::npos ? std::string() : path.substr(0, slash); }
  static bool sameFold(const std::string& a, const std::string& b) { if (a.size() != b.size()) return false; for (size_t i = 0; i < a.size(); ++i) if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false; return true; }
  static std::string leaf(const std::string& path) { const auto slash = path.rfind('/'); return slash == std::string::npos ? path : path.substr(slash + 1); }
  Result run(const Call& call) {
    std::lock_guard lock(mutex); calls.push_back(call);
    Result result; if (!unavailable.empty()) { result.error = unavailable; return result; }
    auto& tree = shares[call.share]; const auto found = tree.find(call.path);
    const auto fail = [&](const char* code) { result.error = code; return result; };
    const auto entry = [&](const std::string& path, const Node& node) { Entry value; value.name = path.empty() ? std::string() : leaf(path); value.dir = node.dir; value.size = node.dir ? 0 : bigSizes.count(path) ? bigSizes[path] : node.data.size(); value.modified = node.modified; value.created = 1761200000000; if (path.empty()) { value.hasSpace = true; value.free = 1000; value.total = 5000; } return value; };
    switch (call.op) {
      case Op::stat: if (found == tree.end() && fold) for (const auto& [path, node] : tree) if (sameFold(path, call.path)) { result.entry = entry(path, node); return result; }
        if (found == tree.end()) return fail("drive_not_found"); result.entry = entry(call.path, found->second); return result;
      case Op::list: {
        if (found == tree.end()) return fail("drive_not_found");
        std::vector<Entry> all; for (const auto& [path, node] : tree) if (!path.empty() && parent(path) == call.path) all.push_back(entry(path, node));
        for (uint32_t i = call.cursor; i < all.size() && i < call.cursor + page; ++i) result.entries.push_back(all[i]);
        if (call.cursor + page < all.size()) result.next = call.cursor + page;
        return result;
      }
      case Op::read: { if (found == tree.end()) return fail("drive_not_found"); const auto& data = found->second.data; if (call.offset < data.size()) result.data.assign(data.begin() + call.offset, data.begin() + std::min<size_t>(data.size(), size_t(call.offset) + call.length)); return result; }
      case Op::write: {
        if (!tree.count(parent(call.path))) return fail("drive_not_found");
        if (found == tree.end() && !call.truncate) return fail("drive_not_found");
        auto& node = tree[call.path]; if (node.dir) return fail("drive_is_directory");
        if (call.truncate) node.data.clear();
        if (node.data.size() < call.offset + call.data.size()) node.data.resize(size_t(call.offset + call.data.size()));
        std::copy(call.data.begin(), call.data.end(), node.data.begin() + call.offset); return result;
      }
      case Op::create:
        if (!tree.count(parent(call.path))) return fail("drive_not_found");
        if (found != tree.end() && (call.dir || !call.truncate)) return fail("drive_exists");
        tree[call.path] = Node{call.dir}; return result;
      case Op::rename: {
        if (found == tree.end()) return fail("drive_not_found");
        if (tree.count(call.to) && !call.replace && !(fold && sameFold(call.path, call.to))) return fail("drive_exists");
        if (!tree.count(parent(call.to))) return fail("drive_not_found");
        const auto node = found->second; tree.erase(found); tree[call.to] = node; return result;
      }
      case Op::remove: {
        if (found == tree.end()) return fail("drive_not_found");
        for (const auto& [path, node] : tree) { (void)node; if (!path.empty() && parent(path) == call.path) return fail("drive_not_empty"); }
        tree.erase(found); return result;
      }
      case Op::touch: if (found == tree.end()) return fail("drive_not_found"); found->second.modified = call.modified; return result;
    }
    return fail("drive_io_failed");
  }
};

// ---- a minimal HTTP client ----
struct Response { int status = 0; std::map<std::string, std::string> headers; std::string body; };
SOCKET connectTo(uint16_t port, bool v6 = false) {
  SOCKET socket = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (v6) { sockaddr_in6 address{}; address.sin6_family = AF_INET6; address.sin6_port = htons(port); address.sin6_addr = in6addr_loopback; if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { closesocket(socket); return INVALID_SOCKET; } }
  else { sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { closesocket(socket); return INVALID_SOCKET; } }
  DWORD timeout = 10000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  return socket;
}
bool sendText(SOCKET socket, const std::string& text) { size_t at = 0; while (at < text.size()) { const int sent = send(socket, text.data() + at, int(std::min<size_t>(text.size() - at, 1 << 16)), 0); if (sent <= 0) return false; at += size_t(sent); } return true; }
// Reads one response (Content-Length or chunked); `head` for HEAD requests.
std::optional<Response> readResponse(SOCKET socket, std::string& buffer, bool head = false) {
  const auto fill = [&] { char data[65536]; const int got = recv(socket, data, sizeof(data), 0); if (got <= 0) return false; buffer.append(data, size_t(got)); return true; };
  size_t end;
  while ((end = buffer.find("\r\n\r\n")) == std::string::npos) if (!fill()) return std::nullopt;
  Response response; std::istringstream lines(buffer.substr(0, end)); std::string line; std::getline(lines, line);
  response.status = std::stoi(line.substr(9, 3));
  while (std::getline(lines, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); const auto colon = line.find(':'); if (colon != std::string::npos) response.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1)); }
  buffer.erase(0, end + 4);
  if (response.status == 100) return readResponse(socket, buffer, head);
  if (head) return response;
  if (lower(response.headers["transfer-encoding"]) == "chunked") {
    for (;;) {
      size_t eol; while ((eol = buffer.find("\r\n")) == std::string::npos) if (!fill()) return std::nullopt;
      const size_t size = std::stoull(buffer.substr(0, eol), nullptr, 16); buffer.erase(0, eol + 2);
      while (buffer.size() < size + 2) if (!fill()) return std::nullopt;
      response.body += buffer.substr(0, size); buffer.erase(0, size + 2);
      if (!size) break;
    }
  } else {
    const size_t length = response.headers.count("content-length") ? std::stoull(response.headers["content-length"]) : 0;
    while (buffer.size() < length) if (!fill()) return std::nullopt;
    response.body = buffer.substr(0, length); buffer.erase(0, length);
  }
  return response;
}
Response http(uint16_t port, const std::string& method, const std::string& target, const std::string& body = {}, std::string headers = {}, bool v6 = false) {
  SOCKET socket = connectTo(port, v6); if (socket == INVALID_SOCKET) return {};
  std::string request = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + headers;
  if (!body.empty() || method == "PUT") request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n" + body;
  sendText(socket, request); std::string buffer;
  auto response = readResponse(socket, buffer, method == "HEAD");
  closesocket(socket);
  return response.value_or(Response{});
}
std::string pattern(size_t size) { std::string out(size, 0); for (size_t i = 0; i < size; ++i) out[i] = char('a' + (i * 7 + i / 251) % 26); return out; }

void names() {
  for (const auto* name : {"a", "Documents", "report.final.docx", "x.y.z", "COM0", "LPT10", "CONSOLE", ".hidden"}) CHECK(safeName(std::string(name)));
  CHECK(safeName(utf8(L"한글 폴더"))); CHECK(safeName(std::string(255, 'x'))); CHECK(!safeName(std::string(256, 'x')));
  for (const auto* name : {"", ".", "..", "a.", "a ", "a/b", "a\\b", "C:", "a*", "a?", "a\"", "a<", "a>", "a|", "CON", "con", "Con.txt", "NUL.tar.gz", "COM1", "lpt9.log", "CONIN$", "conout$.x", "AUX .txt"}) CHECK(!safeName(std::string(name)));
  CHECK(!safeName(std::wstring(L"COM\u00b9"))); CHECK(!safeName(std::wstring(1, wchar_t(0xd800)))); CHECK(!safeName(std::wstring(L"a\x1f")));
  for (const auto* path : {"", "a", "a/b/c"}) CHECK(safePath(path));
  for (const auto* path : {"/", "/a", "a/", "a//b", "..", "a/..", "a/../b", "./a", "a\\b", "C:/Windows", "a/CON"}) CHECK(!safePath(path));
  std::string deep; for (int i = 0; i < 65; ++i) deep += (i ? "/" : "") + std::string("d"); CHECK(!safePath(deep));
  CHECK(shareId("docs") && shareId("a_b-1") && !shareId("") && !shareId("a b") && !shareId(std::string(33, 'a')));
  const auto token = randomToken(); CHECK(token.size() == 43 && validToken(token)); CHECK(token != randomToken());
  CHECK(shareToken(std::string(sharePrefix) + token)); CHECK(!shareToken(token)); CHECK(!shareToken(std::string(sharePrefix) + token.substr(1)));
  CHECK(httpStatus("drive_not_found") == 404 && httpStatus("drive_read_only") == 403 && httpStatus("drive_busy") == 423 && httpStatus("drive_unavailable") == 503 && httpStatus("drive_timeout") == 504 && httpStatus("drive_io_failed") == 502);
}

void targets() {
  const auto token = std::string(sharePrefix) + randomToken();
  auto target = parseTarget("/" + token + "/%ED%95%9C%EA%B8%80/b%20c.txt"); CHECK(target && target->token == token && target->path == utf8(L"한글") + "/b c.txt");
  CHECK(href(token, target->path, false) == "/" + token + "/%ED%95%9C%EA%B8%80/b%20c.txt");
  CHECK(href(token, "", true) == "/" + token + "/");
  target = parseTarget("http://localhost:4567/" + token + "/a/"); CHECK(target && target->path == "a");
  target = parseTarget("http://127.0.0.1/" + token); CHECK(target && target->path.empty());
  CHECK(parseTarget("/") && parseTarget("/")->root); CHECK(parseTarget("*") && parseTarget("*")->root);
  for (const auto& bad : std::vector<std::string>{"/" + token + "/..", "/" + token + "/a/../b", "/" + token + "/a%2Fb", "/" + token + "/a%5Cb", "/" + token + "/a%00", "/" + token + "/%FF", "/" + token + "/a//b",
                          "/" + token + "/a?x", "/" + token + "/CON", "/Multitap-short/a", "/other/a", "http://evil.example/" + token, "relative", "/" + token + "/a%2", "/" + token + "/a b"})
    CHECK(!parseTarget(bad));
  CHECK(httpDate(0) == "Thu, 01 Jan 1970 00:00:00 GMT"); CHECK(httpDate(1761300000000) == "Fri, 24 Oct 2025 10:00:00 GMT");
  CHECK(isoDate(1761300000000) == "2025-10-24T10:00:00Z");
  CHECK(parseHttpDate("Fri, 24 Oct 2025 10:00:00 GMT") == std::optional<int64_t>(1761300000000)); CHECK(!parseHttpDate("not a date"));
}

void dav() {
  MemoryBackend backend; DavServer server(backend, std::chrono::milliseconds(3000));
  const auto port = server.start(); CHECK(port != 0);
  const auto docs = std::string(sharePrefix) + randomToken(), pics = std::string(sharePrefix) + randomToken(), unknown = std::string(sharePrefix) + randomToken();
  server.setShare(docs, {"docs", false}); server.setShare(pics, {"pics", true});
  backend.file("docs", "a.txt", "hello world"); backend.dir("docs", "sub"); backend.file("docs", "sub/x.bin", "x"); backend.file("docs", utf8(L"한글.txt"), "k"); backend.file("docs", "z.txt", "z");
  const auto big = pattern(600000); backend.file("docs", "big.bin", big);
  backend.file("pics", "p.jpg", "jpeg");

  auto response = http(port, "OPTIONS", "/"); CHECK(response.status == 200 && response.headers["dav"] == "1, 2" && response.headers["allow"].find("PROPFIND") != std::string::npos);
  response = http(port, "OPTIONS", "/", {}, {}, true); CHECK(response.status == 200 || response.status == 0); // ::1 when IPv6 is available
  response = http(port, "PROPFIND", "/" + unknown + "/", {}, "Depth: 0\r\n"); CHECK(response.status == 404);
  response = http(port, "PROPFIND", "/", {}, "Depth: 1\r\n"); CHECK(response.status == 207 && response.body.find("<D:href>/</D:href>") != std::string::npos && response.body.find(sharePrefix) == std::string::npos);
  response = http(port, "PROPFIND", "/" + docs, {}, "Depth: 0\r\n");
  CHECK(response.status == 207 && response.body.find("<D:href>/" + docs + "/</D:href>") != std::string::npos && response.body.find("<D:collection/>") != std::string::npos && response.body.find("quota-available-bytes>1000<") != std::string::npos);
  CHECK(response.body.find("a.txt") == std::string::npos);
  response = http(port, "PROPFIND", "/" + docs + "/", "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:allprop/></D:propfind>", "Depth: 1\r\n");
  CHECK(response.status == 207 && lower(response.headers["transfer-encoding"]) == "chunked");
  for (const auto& child : {"/a.txt<", "/sub/<", "/big.bin<", "/%ED%95%9C%EA%B8%80.txt<", "/z.txt<"}) CHECK(response.body.find("<D:href>/" + docs + child) != std::string::npos);
  CHECK(response.body.find("/sub/x.bin") == std::string::npos);
  CHECK(response.body.find("<D:getcontentlength>11</D:getcontentlength>") != std::string::npos && response.body.find("<Z:Win32FileAttributes>00000010</Z:Win32FileAttributes>") != std::string::npos);
  CHECK(response.body.rfind("</D:multistatus>") != std::string::npos);
  response = http(port, "PROPFIND", "/" + pics + "/", {}, "Depth: 1\r\n");
  CHECK(response.status == 207 && response.body.find("<Z:Win32FileAttributes>00000021</Z:Win32FileAttributes>") != std::string::npos);
  response = http(port, "PROPFIND", "/" + docs + "/missing", {}, "Depth: 0\r\n"); CHECK(response.status == 404);

  response = http(port, "GET", "/" + docs + "/a.txt"); CHECK(response.status == 200 && response.body == "hello world" && response.headers["content-length"] == "11" && response.headers["accept-ranges"] == "bytes");
  response = http(port, "GET", "/" + docs + "/a.txt", {}, "Range: bytes=2-5\r\n"); CHECK(response.status == 206 && response.body == "llo " && response.headers["content-range"] == "bytes 2-5/11");
  response = http(port, "GET", "/" + docs + "/a.txt", {}, "Range: bytes=-3\r\n"); CHECK(response.status == 206 && response.body == "rld");
  response = http(port, "GET", "/" + docs + "/a.txt", {}, "Range: bytes=20-\r\n"); CHECK(response.status == 416 && response.headers["content-range"] == "bytes */11");
  response = http(port, "GET", "/" + docs + "/big.bin"); CHECK(response.status == 200 && response.body == big);
  response = http(port, "GET", "/" + docs + "/big.bin", {}, "Range: bytes=262000-600000\r\n"); CHECK(response.status == 206 && response.body == big.substr(262000));
  response = http(port, "HEAD", "/" + docs + "/big.bin"); CHECK(response.status == 200 && response.headers["content-length"] == "600000" && response.body.empty());
  response = http(port, "GET", "/" + docs + "/sub"); CHECK(response.status == 403);
  response = http(port, "GET", "/" + docs + "/nope"); CHECK(response.status == 404);

  // Keep-alive: two requests on one connection.
  { SOCKET socket = connectTo(port); std::string buffer;
    sendText(socket, "GET /" + docs + "/a.txt HTTP/1.1\r\nHost: localhost\r\n\r\nGET /" + docs + "/z.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto first = readResponse(socket, buffer), second = readResponse(socket, buffer);
    CHECK(first && first->body == "hello world" && second && second->body == "z"); closesocket(socket); }

  // PUT: new (201), replace (204), chunked, empty, read-only.
  const auto upload = pattern(700000);
  { SOCKET socket = connectTo(port); std::string buffer;
    sendText(socket, "PUT /" + docs + "/new.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nExpect: 100-continue\r\nContent-Length: " + std::to_string(upload.size()) + "\r\n\r\n");
    char data[64]; const int got = recv(socket, data, sizeof(data), 0); CHECK(got > 0 && std::string(data, size_t(got)).rfind("HTTP/1.1 100 Continue", 0) == 0);
    sendText(socket, upload); const auto result = readResponse(socket, buffer); CHECK(result && result->status == 201); closesocket(socket); }
  CHECK(backend.content("docs", "new.bin") == upload);
  response = http(port, "PUT", "/" + docs + "/new.bin", "short"); CHECK(response.status == 204 && backend.content("docs", "new.bin") == "short");
  { SOCKET socket = connectTo(port); std::string buffer;
    sendText(socket, "PUT /" + docs + "/chunked.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n");
    const auto result = readResponse(socket, buffer); CHECK(result && result->status == 201); closesocket(socket); }
  CHECK(backend.content("docs", "chunked.txt") == "hello world");
  response = http(port, "PUT", "/" + docs + "/empty.txt", ""); CHECK(response.status == 201 && backend.exists("docs", "empty.txt") && backend.content("docs", "empty.txt").empty());
  response = http(port, "PUT", "/" + pics + "/p.jpg", "overwrite"); CHECK(response.status == 403 && backend.content("pics", "p.jpg") == "jpeg");
  response = http(port, "PUT", "/" + docs + "/sub", "x"); CHECK(response.status == 405);
  response = http(port, "PUT", "/" + docs + "/missing/x.txt", "x"); CHECK(response.status == 404);

  // MKCOL, MOVE, DELETE, COPY, read-only.
  response = http(port, "MKCOL", "/" + docs + "/made"); CHECK(response.status == 201 && backend.exists("docs", "made"));
  response = http(port, "MKCOL", "/" + docs + "/made"); CHECK(response.status == 405);
  response = http(port, "MKCOL", "/" + docs + "/no/such"); CHECK(response.status == 409);
  response = http(port, "MKCOL", "/" + pics + "/x"); CHECK(response.status == 403 && !backend.exists("pics", "x"));
  response = http(port, "MOVE", "/" + docs + "/a.txt", {}, "Destination: http://localhost:" + std::to_string(port) + "/" + docs + "/made/b.txt\r\n");
  CHECK(response.status == 201 && backend.content("docs", "made/b.txt") == "hello world" && !backend.exists("docs", "a.txt"));
  response = http(port, "MOVE", "/" + docs + "/z.txt", {}, "Destination: /" + docs + "/made/b.txt\r\nOverwrite: F\r\n"); CHECK(response.status == 412 && backend.exists("docs", "z.txt"));
  response = http(port, "MOVE", "/" + docs + "/z.txt", {}, "Destination: /" + docs + "/made/b.txt\r\n"); CHECK(response.status == 204 && backend.content("docs", "made/b.txt") == "z");
  response = http(port, "MOVE", "/" + docs + "/made/b.txt", {}, "Destination: /" + pics + "/b.txt\r\n"); CHECK(response.status == 502);
  response = http(port, "MOVE", "/" + docs + "/made/b.txt", {}, "Destination: /" + docs + "/../b.txt\r\n"); CHECK(response.status == 400);
  // A case-only rename (Explorer sends Overwrite: F): the destination "exists" as the same file, which is a rename, not an overwrite.
  backend.fold = true; backend.file("docs", "Report.txt", "case");
  response = http(port, "MOVE", "/" + docs + "/Report.txt", {}, "Destination: /" + docs + "/report.txt\r\nOverwrite: F\r\n");
  CHECK(response.status == 201 && backend.exists("docs", "report.txt") && !backend.exists("docs", "Report.txt") && backend.content("docs", "report.txt") == "case");
  CHECK(!backend.calls.back().replace && backend.calls.back().op == Op::rename);
  // A different file under another capitalisation is still an overwrite: refused with Overwrite: F.
  backend.file("docs", "other.txt", "o");
  response = http(port, "MOVE", "/" + docs + "/other.txt", {}, "Destination: /" + docs + "/REPORT.txt\r\nOverwrite: F\r\n");
  CHECK(response.status == 412 && backend.exists("docs", "other.txt") && backend.content("docs", "report.txt") == "case");
  backend.fold = false;
  response = http(port, "DELETE", "/" + docs + "/report.txt"); CHECK(response.status == 204);
  response = http(port, "DELETE", "/" + docs + "/other.txt"); CHECK(response.status == 204);
  response = http(port, "DELETE", "/" + docs + "/made"); CHECK(response.status == 403 && backend.exists("docs", "made"));
  response = http(port, "DELETE", "/" + docs + "/made/b.txt"); CHECK(response.status == 204 && !backend.exists("docs", "made/b.txt"));
  response = http(port, "DELETE", "/" + docs + "/made"); CHECK(response.status == 204 && !backend.exists("docs", "made"));
  response = http(port, "DELETE", "/" + docs + "/"); CHECK(response.status == 403);
  response = http(port, "DELETE", "/" + pics + "/p.jpg"); CHECK(response.status == 403 && backend.exists("pics", "p.jpg"));
  response = http(port, "COPY", "/" + docs + "/big.bin", {}, "Destination: /" + docs + "/c.bin\r\n"); CHECK(response.status == 501);

  // LOCK creates a missing file (MRxDAV's new-file sequence); UNLOCK; PROPPATCH applies the modified time.
  response = http(port, "LOCK", "/" + docs + "/locked.docx", "<?xml version=\"1.0\"?><D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockinfo>", "Timeout: Second-3600\r\n");
  CHECK(response.status == 201 && response.headers["lock-token"].rfind("<opaquelocktoken:", 0) == 0 && response.body.find("<D:lockdiscovery>") != std::string::npos && backend.exists("docs", "locked.docx"));
  response = http(port, "LOCK", "/" + docs + "/locked.docx", "<D:lockinfo xmlns:D=\"DAV:\"/>"); CHECK(response.status == 200);
  response = http(port, "UNLOCK", "/" + docs + "/locked.docx", {}, "Lock-Token: <opaquelocktoken:x>\r\n"); CHECK(response.status == 204);
  response = http(port, "LOCK", "/" + pics + "/p.jpg", "<D:lockinfo xmlns:D=\"DAV:\"/>"); CHECK(response.status == 403);
  response = http(port, "PROPPATCH", "/" + docs + "/big.bin", "<?xml version=\"1.0\" encoding=\"utf-8\" ?><D:propertyupdate xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\"><D:set><D:prop><Z:Win32CreationTime>Fri, 24 Oct 2025 09:00:00 GMT</Z:Win32CreationTime><Z:Win32LastModifiedTime>Fri, 24 Oct 2025 10:00:00 GMT</Z:Win32LastModifiedTime><Z:Win32FileAttributes>00000020</Z:Win32FileAttributes></D:prop></D:set></D:propertyupdate>");
  CHECK(response.status == 207 && response.body.find("<Z:Win32LastModifiedTime/>") != std::string::npos && response.body.find("<Z:Win32CreationTime/>") != std::string::npos);
  { std::lock_guard lock(backend.mutex); CHECK(!backend.calls.empty() && backend.calls.back().op == Op::touch && backend.calls.back().modified == 1761300000000 && backend.calls.back().path == "big.bin"); }

  // Escapes and malformed requests never reach the backend.
  size_t before; { std::lock_guard lock(backend.mutex); before = backend.calls.size(); }
  for (const auto& bad : {"/" + docs + "/..%2Fx", "/" + docs + "/a%5Cb", "/" + docs + "/CON", "/" + docs + "/%2e%2e/x", "/" + docs + "/a?x=1"}) { response = http(port, "GET", bad); CHECK(response.status == 400); }
  response = http(port, "GET", "/" + docs + "/a.txt", {}, "X-Big: " + std::string(20000, 'x') + "\r\n"); CHECK(response.status == 431);
  { std::lock_guard lock(backend.mutex); CHECK(backend.calls.size() == before); }
  response = http(port, "PUT", "/" + docs + "/x", {}, "Transfer-Encoding: gzip\r\n"); CHECK(response.status == 501 || response.status == 400);

  // A revoked token is gone at once; an unavailable or slow client is 503 / 504.
  server.removeShare(pics); response = http(port, "GET", "/" + pics + "/p.jpg"); CHECK(response.status == 404);
  backend.unavailable = "drive_unavailable"; response = http(port, "GET", "/" + docs + "/big.bin"); CHECK(response.status == 503); backend.unavailable.clear();
  { MemoryBackend slow; slow.delay = std::chrono::milliseconds(800); DavServer quick(slow, std::chrono::milliseconds(200)); const auto other = quick.start(); quick.setShare(docs, {"docs", false});
    response = http(other, "PROPFIND", "/" + docs + "/", {}, "Depth: 0\r\n"); CHECK(response.status == 504); quick.stop(); }
  server.stop();
  CHECK(connectTo(port) == INVALID_SOCKET);
}

void requests() {
  std::vector<std::string> texts; std::vector<std::vector<uint8_t>> binaries; std::mutex mutex;
  Requests multiplexer([&](std::string text) { std::lock_guard lock(mutex); texts.push_back(std::move(text)); return true; }, [&](std::vector<uint8_t> data) { std::lock_guard lock(mutex); binaries.push_back(std::move(data)); return true; }, std::chrono::milliseconds(150));
  Call stat; stat.op = Op::stat; stat.share = "docs"; stat.path = utf8(L"한글/a.txt");
  auto first = multiplexer.submit(stat);
  CHECK(texts.size() == 1 && texts[0] == "{\"type\":\"drive.request\",\"id\":1,\"op\":\"stat\",\"share\":\"docs\",\"path\":\"" + utf8(L"한글/a.txt") + "\"}");
  CHECK(multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":1,\"ok\":true,\"entry\":{\"name\":\"a.txt\",\"dir\":false,\"size\":3,\"modified\":1761300000000,\"created\":1761200000000,\"readOnly\":true}}")));
  auto result = first.get(); CHECK(result.ok() && result.entry.name == "a.txt" && result.entry.size == 3 && result.entry.readOnly && result.entry.modified == 1761300000000);
  Call list; list.op = Op::list; list.share = "docs"; list.cursor = 4;
  auto listed = multiplexer.submit(list); CHECK(texts.back() == "{\"type\":\"drive.request\",\"id\":2,\"op\":\"list\",\"share\":\"docs\",\"path\":\"\",\"cursor\":4}");
  CHECK(!multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":2,\"ok\":true,\"entries\":[{\"name\":\"..\",\"dir\":true,\"size\":0,\"modified\":0,\"created\":0,\"readOnly\":false}]}")));
  CHECK(multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":2,\"ok\":true,\"entries\":[{\"name\":\"b\",\"dir\":true,\"size\":0,\"modified\":0,\"created\":0,\"readOnly\":false}],\"next\":5}")));
  result = listed.get(); CHECK(result.ok() && result.entries.size() == 1 && result.entries[0].dir && result.next == std::optional<uint32_t>(5));
  Call read; read.op = Op::read; read.share = "docs"; read.path = "a"; read.offset = 10; read.length = 4;
  auto data = multiplexer.submit(read); CHECK(texts.back() == "{\"type\":\"drive.request\",\"id\":3,\"op\":\"read\",\"share\":\"docs\",\"path\":\"a\",\"offset\":10,\"length\":4}");
  CHECK(!multiplexer.receiveBinary({1, 3, 0, 0, 0, 1, 2, 3, 4, 5}));
  CHECK(multiplexer.receiveBinary({1, 3, 0, 0, 0, 7, 8}));
  result = data.get(); CHECK(result.ok() && result.data == std::vector<uint8_t>({7, 8}));
  Call write; write.op = Op::write; write.share = "docs"; write.path = "w"; write.offset = 0; write.truncate = true; write.data = {9, 9, 9};
  auto written = multiplexer.submit(write);
  const std::string header = "{\"op\":\"write\",\"share\":\"docs\",\"path\":\"w\",\"offset\":0,\"truncate\":true}";
  CHECK(binaries.size() == 1 && binaries[0].size() == 7 + header.size() + 3 && binaries[0][0] == 2 && binaries[0][1] == 4 && binaries[0][5] == header.size() && std::string(binaries[0].begin() + 7, binaries[0].begin() + 7 + header.size()) == header && binaries[0].back() == 9);
  CHECK(multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":4,\"ok\":false,\"error\":\"drive_read_only\"}")));
  CHECK(written.get().error == "drive_read_only");
  // Outside the contract: unknown id, unknown code, a read answered as JSON.
  CHECK(!multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":99,\"ok\":true}")));
  auto again = multiplexer.submit(read);
  CHECK(!multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":5,\"ok\":false,\"error\":\"C:\\\\secret\"}")));
  CHECK(!multiplexer.receive(JsonParser::parse("{\"type\":\"drive.response\",\"id\":5,\"ok\":true}")));
  // At most 8 in flight: the ninth waits, then times out.
  std::vector<std::future<Result>> waiting; for (int i = 0; i < 7; ++i) waiting.push_back(multiplexer.submit(stat));
  CHECK(multiplexer.inFlight() == 8);
  auto ninth = multiplexer.submit(stat); CHECK(ninth.get().error == "drive_timeout"); CHECK(multiplexer.inFlight() == 8);
  multiplexer.close();
  CHECK(again.get().error == "drive_unavailable"); for (auto& future : waiting) CHECK(future.get().error == "drive_unavailable");
  CHECK(multiplexer.submit(stat).get().error == "drive_unavailable");
}

void mounts() {
  const auto token = std::string(sharePrefix) + randomToken();
  const auto remote = remoteName(51234, token); CHECK(remote == L"\\\\localhost@51234\\" + wide(token));
  CHECK(ownToken(remote) == std::optional<std::string>(token)); CHECK(ownToken(L"\\\\LOCALHOST@1\\" + wide(token)).has_value());
  for (const auto& other : std::vector<std::wstring>{L"\\\\localhost@51234\\DavWWWRoot", L"\\\\server\\share", L"\\\\localhost\\" + wide(token), L"\\\\localhost@123456\\" + wide(token), L"\\\\localhost@12a\\" + wide(token), L"\\\\localhost@51234\\" + wide(token) + L"\\sub", L"\\\\other@51234\\" + wide(token)})
    CHECK(!ownToken(other));
  CHECK(labelKey(remote) == L"##localhost@51234#" + wide(token));
  CHECK(freeLetter(0, {}) == std::optional<wchar_t>(L'Z'));
  CHECK(freeLetter((1u << 25) | (1u << 24), {}) == std::optional<wchar_t>(L'X'));
  CHECK(freeLetter(0, {L'Z', L'Y'}) == std::optional<wchar_t>(L'X'));
  CHECK(!freeLetter(0x03fffff8u, {})); // D..Z taken
  CHECK(driveLabel("Documents") == L"Documents (PC)");
  // The helper refuses anything outside its contract before touching Windows.
  for (const auto* bad : {"{\"op\":\"format\"}", "{\"op\":\"map\",\"port\":80,\"shares\":[]}", "{\"op\":\"map\",\"port\":51234,\"shares\":[{\"token\":\"x\",\"label\":\"a\"}]}", "{\"op\":\"unmap\",\"tokens\":[\"\\\\\\\\server\"]}"}) {
    bool refused = false; try { (void)driveMountCommand(JsonParser::parse(bad)); } catch (...) { refused = true; } CHECK(refused);
  }
  static_assert(fileSizeLimit == 0xffffffffu && attributesLimit == 10000000u);
}

// ---- the orchestrator with a fake relay and a fake mounter ----
class FakeLink final : public Link {
 public:
  std::mutex mutex; std::condition_variable changed; std::vector<std::string> texts; std::vector<std::vector<uint8_t>> binaries; bool aborted = false, started = false, refuse = false; int starts = 0;
  Channel::Text text; Channel::Binary binary; Channel::Closed closed;
  // A dial starts a fresh socket (channel-resume-v1: the same link is dialled again).
  void start(const Credentials&, const std::string&, const std::string&, Channel::Text t, Channel::Binary b, Channel::Closed c) override { { std::lock_guard lock(mutex); aborted = false; texts.clear(); } text = std::move(t); binary = std::move(b); closed = std::move(c); started = true; ++starts; }
  bool refused() const override { return refuse; }
  bool sendText(std::string value) override { std::lock_guard lock(mutex); if (aborted) return false; texts.push_back(std::move(value)); changed.notify_all(); return true; }
  bool sendBinary(std::vector<uint8_t> value) override { std::lock_guard lock(mutex); if (aborted) return false; binaries.push_back(std::move(value)); changed.notify_all(); return true; }
  void abort() override { { std::lock_guard lock(mutex); aborted = true; } changed.notify_all(); }
  void stop() override { abort(); if (closed) closed(); }
  std::optional<std::string> wait(const std::function<bool(const std::string&)>& match, int ms = 5000) {
    std::unique_lock lock(mutex);
    std::optional<std::string> found;
    changed.wait_for(lock, std::chrono::milliseconds(ms), [&] { for (const auto& item : texts) if (match(item)) { found = item; return true; } return false; });
    return found;
  }
};
class FakeMounter final : public Mounter {
 public:
  std::mutex mutex; std::vector<std::vector<std::string>> unmapped; std::vector<std::vector<std::pair<std::string, std::string>>> mapped; int prepared = 0, swept = 0; std::string prepareError; char next = 'Z'; bool old = false;
  std::string prepare() override { std::lock_guard lock(mutex); ++prepared; return prepareError; }
  bool oldLimit() override { return old; }
  void sweep(DWORD) override { std::lock_guard lock(mutex); ++swept; }
  std::vector<MountResult> map(DWORD, uint16_t port, const std::vector<std::pair<std::string, std::string>>& shares) override {
    std::lock_guard lock(mutex); mapped.push_back(shares); std::vector<MountResult> out;
    for (const auto& [token, label] : shares) out.push_back(label == "Full" ? MountResult{token, {}, "drive_letter_unavailable"} : MountResult{token, std::string(1, next--), {}});
    (void)port; return out;
  }
  void unmap(DWORD, const std::vector<std::string>& tokens) override { std::lock_guard lock(mutex); unmapped.push_back(tokens); }
};
bool eventually(const std::function<bool()>& condition, int ms = 5000) { for (int i = 0; i < ms / 10; ++i) { if (condition()) return true; Sleep(10); } return condition(); }

void orchestrator() {
  auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>();
  auto& link = *linkOwned; auto& mounter = *mounterOwned;
  Redirection redirection(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000));
  Credentials credentials; credentials.origin = L"https://broker.example"; credentials.token = std::string(43, 'a');
  const std::string session = "11111111-2222-3333-4444-555555555555";
  redirection.start(credentials, session, "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive", 1);
  CHECK(link.started);
  link.text("{\"type\":\"drive.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"drive-v1\",\"limits\":{\"maxShares\":8,\"chunkBytes\":262144,\"maxInFlight\":8,\"listPage\":256,\"maxPath\":1024,\"maxName\":255,\"maxDepth\":64}}");
  CHECK(eventually([&] { return redirection.port() != 0; }));
  link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false},{\"id\":\"pics\",\"name\":\"Pictures\",\"readOnly\":true},{\"id\":\"full\",\"name\":\"Full\",\"readOnly\":false}]}");
  const auto mounted = link.wait([](const std::string& text) { return text.find("\"state\":\"mapped\"") != std::string::npos; });
  CHECK(mounted && mounted->find("{\"id\":\"docs\",\"state\":\"mapped\",\"letter\":\"Z\"}") != std::string::npos && mounted->find("{\"id\":\"full\",\"state\":\"failed\",\"error\":\"drive_letter_unavailable\"}") != std::string::npos);
  CHECK(link.wait([](const std::string& text) { return text.find("\"state\":\"pending\"") != std::string::npos; }).has_value());
  { std::lock_guard lock(mounter.mutex); CHECK(mounter.prepared == 1 && mounter.swept == 1 && mounter.mapped.size() == 1 && mounter.mapped[0].size() == 3); }
  const auto docs = redirection.token("docs"), pics = redirection.token("pics");
  CHECK(shareToken(docs) && shareToken(pics) && docs != pics);
  // A WebDAV request becomes a drive.request on the relay; the answer comes back.
  std::thread client([&] {
    const auto request = link.wait([](const std::string& text) { return text.find("\"type\":\"drive.request\"") != std::string::npos; });
    CHECK(request && request->find("\"op\":\"stat\",\"share\":\"docs\",\"path\":\"\"") != std::string::npos);
    link.text("{\"type\":\"drive.response\",\"id\":1,\"ok\":true,\"entry\":{\"name\":\"\",\"dir\":true,\"size\":0,\"modified\":1761300000000,\"created\":1761200000000,\"readOnly\":false}}");
  });
  auto response = http(redirection.port(), "PROPFIND", "/" + docs + "/", {}, "Depth: 0\r\n");
  client.join();
  CHECK(response.status == 207 && response.body.find("<D:href>/" + docs + "/</D:href>") != std::string::npos);
  response = http(redirection.port(), "PUT", "/" + pics + "/x.txt", "x"); CHECK(response.status == 403);
  // Withdrawn by the client: access ends at once, the letter goes next.
  link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":true}]}");
  response = http(redirection.port(), "GET", "/" + pics + "/a"); CHECK(response.status == 404);
  response = http(redirection.port(), "PUT", "/" + docs + "/x.txt", "x"); CHECK(response.status == 403);
  CHECK(eventually([&] { std::lock_guard lock(mounter.mutex); return mounter.unmapped.size() == 1; }));
  { std::lock_guard lock(mounter.mutex); CHECK(mounter.unmapped.size() == 1 && mounter.unmapped[0] == std::vector<std::string>{pics}); }
  // A malformed message ends the channel; ending it revokes and unmaps everything.
  link.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"bad id\",\"name\":\"x\",\"readOnly\":false}]}");
  CHECK(link.aborted);
  link.closed();
  response = http(redirection.port(), "GET", "/" + docs + "/a"); CHECK(response.status == 404);
  CHECK(eventually([&] { std::lock_guard lock(mounter.mutex); return mounter.unmapped.size() == 2; }));
  { std::lock_guard lock(mounter.mutex); CHECK(mounter.unmapped.size() == 2 && mounter.unmapped[1] == std::vector<std::string>{docs}); }
  redirection.stop();
  { std::lock_guard lock(mounter.mutex); CHECK(mounter.unmapped.size() == 2); }

  // A wrong session or protocol in drive.ready is refused; WebClient missing fails every share.
  auto secondLink = std::make_unique<FakeLink>(); auto secondMounter = std::make_unique<FakeMounter>(); secondMounter->prepareError = "drive_webclient_unavailable";
  auto& link2 = *secondLink; auto& mounter2 = *secondMounter;
  Redirection second(std::move(secondLink), std::move(secondMounter), std::chrono::milliseconds(1000));
  second.start(credentials, session, "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive", 1);
  link2.text("{\"type\":\"drive.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"drive-v1\",\"limits\":{\"chunkBytes\":262144,\"maxInFlight\":8}}");
  link2.text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false}]}");
  CHECK(link2.wait([](const std::string& text) { return text.find("\"error\":\"drive_webclient_unavailable\"") != std::string::npos; }).has_value());
  { std::lock_guard lock(mounter2.mutex); CHECK(mounter2.mapped.empty() && mounter2.swept == 0); }
  link2.text("{\"type\":\"drive.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"drive-v1\",\"limits\":{\"chunkBytes\":262144,\"maxInFlight\":8}}");
  CHECK(link2.aborted); // a second ready is outside the contract
  second.stop();
  auto thirdLink = std::make_unique<FakeLink>(); auto& link3 = *thirdLink;
  Redirection third(std::move(thirdLink), std::make_unique<FakeMounter>());
  third.start(credentials, session, "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive", 1);
  link3.text("{\"type\":\"drive.ready\",\"sessionId\":\"other\",\"protocol\":\"drive-v1\",\"limits\":{\"chunkBytes\":262144,\"maxInFlight\":8}}");
  CHECK(link3.aborted && third.port() == 0);
  third.stop();
  CHECK(Channel::validUrl(credentials, session, "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive"));
  CHECK(!Channel::validUrl(credentials, session, "wss://evil.example/api/v1/hosts/sessions/" + session + "/drive"));
  CHECK(!Channel::validUrl(credentials, session, "wss://broker.example/api/v1/hosts/sessions/" + session + "/clipboard"));
}

// ---- channel-resume-v1: the orchestrator's own end is reported, and redial() pairs the same session again ----
void redials() {
  auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>();
  auto& link = *linkOwned; auto& mounter = *mounterOwned;
  Redirection redirection(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000));
  std::mutex mutex; std::vector<bool> ends;
  redirection.onEnded([&](bool transient) { std::lock_guard lock(mutex); ends.push_back(transient); });
  const auto ended = [&] { std::lock_guard lock(mutex); return ends; };
  Credentials credentials; credentials.origin = L"https://broker.example"; credentials.token = std::string(43, 'a');
  const std::string session = "11111111-2222-3333-4444-555555555555", url = "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive";
  const std::string ready = "{\"type\":\"drive.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"drive-v1\",\"limits\":{\"maxShares\":8,\"chunkBytes\":262144,\"maxInFlight\":8,\"listPage\":256,\"maxPath\":1024,\"maxName\":255,\"maxDepth\":64}}";
  const std::string shares = "{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false},{\"id\":\"pics\",\"name\":\"Pictures\",\"readOnly\":true}]}";
  const std::string both = "{\"id\":\"docs\",\"state\":\"mapped\",\"letter\":\"Z\"},{\"id\":\"pics\",\"state\":\"mapped\",\"letter\":\"Y\"}";
  const auto mappedBoth = [&] { return link.wait([&](const std::string& text) { return text.find(both) != std::string::npos; }).has_value(); };
  // A channel that never started has nothing to dial again.
  bool threw = false; try { redirection.redial(); } catch (...) { threw = true; } CHECK(threw && !link.started);
  redirection.start(credentials, session, url, 1);
  link.text(ready); CHECK(eventually([&] { return redirection.port() != 0; }));
  link.text(shares); CHECK(mappedBoth());
  const auto docs = redirection.token("docs"), pics = redirection.token("pics");
  // The Broker closes the pair because the client is away: reported as transient, and everything of the pair
  // goes exactly as before (access revoked at once, both letters unmapped).
  link.text("{\"type\":\"drive.error\",\"reason\":\"drive_stream_away\"}"); CHECK(link.aborted);
  link.closed();
  CHECK(ended() == std::vector<bool>{true});
  auto response = http(redirection.port(), "GET", "/" + docs + "/a"); CHECK(response.status == 404);
  CHECK(eventually([&] { std::lock_guard lock(mounter.mutex); return mounter.unmapped.size() == 1; }));
  { std::lock_guard lock(mounter.mutex); CHECK(mounter.unmapped.size() == 1 && mounter.unmapped[0].size() == 2); mounter.next = 'Z'; } // the letters are free again
  // The same session is dialled again; the client offers the same folders and they get the same letters,
  // with fresh tokens (nothing of the ended pair is reused).
  redirection.redial();
  CHECK(link.starts == 2 && !link.aborted);
  link.text(ready); link.text(shares); CHECK(mappedBoth());
  CHECK(shareToken(redirection.token("docs")) && redirection.token("docs") != docs && redirection.token("pics") != pics);
  { std::lock_guard lock(mounter.mutex); CHECK(mounter.mapped.size() == 2 && mounter.mapped[1].size() == 2 && mounter.mapped[1][0].second == "Documents" && mounter.mapped[1][1].second == "Pictures"); }
  // A request in flight when the pair ends fails at once on the desktop side (no hang), and works on the next pair.
  std::thread dropped([&] {
    CHECK(link.wait([](const std::string& text) { return text.find("\"type\":\"drive.request\"") != std::string::npos; }).has_value());
    link.text("{\"type\":\"drive.error\",\"reason\":\"drive_transport_closed\"}"); link.closed();
  });
  const auto before = GetTickCount64();
  response = http(redirection.port(), "PROPFIND", "/" + redirection.token("docs") + "/", {}, "Depth: 0\r\n");
  dropped.join();
  CHECK(response.status != 207 && GetTickCount64() - before < 2500);
  CHECK(ended() == (std::vector<bool>{true, true}));
  { std::lock_guard lock(mounter.mutex); mounter.next = 'Z'; }
  redirection.redial(); link.text(ready); link.text(shares); CHECK(mappedBoth());
  std::thread answered([&] {
    const auto request = link.wait([](const std::string& text) { return text.find("\"type\":\"drive.request\"") != std::string::npos; });
    CHECK(request.has_value());
    const auto id = request ? JsonParser::parse(*request).get("id").number(0) : 0;
    link.text("{\"type\":\"drive.response\",\"id\":" + std::to_string(int(id)) + ",\"ok\":true,\"entry\":{\"name\":\"\",\"dir\":true,\"size\":0,\"modified\":1761300000000,\"created\":1761200000000,\"readOnly\":false}}");
  });
  response = http(redirection.port(), "PROPFIND", "/" + redirection.token("docs") + "/", {}, "Depth: 0\r\n");
  answered.join();
  CHECK(response.status == 207);
  // A reason of the pair's own (the permission was taken away) is final.
  link.text("{\"type\":\"drive.error\",\"reason\":\"drive_policy_changed\"}"); link.closed();
  CHECK(ended() == (std::vector<bool>{true, true, false}));
  // The end stop() causes is the session's end: not reported.
  redirection.redial(); link.text(ready); redirection.stop();
  CHECK(ended().size() == 3);
  // A socket that just ends is a transport end; a dial the Broker refused is final.
  redirection.redial(); link.closed();
  CHECK(ended() == (std::vector<bool>{true, true, false, true}));
  link.refuse = true; redirection.redial(); link.closed();
  CHECK(ended() == (std::vector<bool>{true, true, false, true, false}));
  redirection.stop();
  // Without a listener (a session that did not negotiate) nothing changes: the end is only the end.
  redirection.onEnded({});
  link.refuse = false; redirection.redial(); link.closed(); redirection.stop();
  CHECK(ended().size() == 5);
}

// ---- WebClient preparation: every service state against a fake service layer ----
class FakeWebClient final : public WebClientSystem {
 public:
  WebClientState current = WebClientState::running; bool raised = false, newer = false, other = false, startOk = true, restartOk = true, stoppedByFailedRestart = false, runsAfterWait = true;
  int starts = 0, restarts = 0, usedChecks = 0;
  WebClientState state() override { return current; }
  bool raiseLimits() override { return raised; }
  bool limitsNewerThanService() override { return newer; }
  bool otherUse() override { ++usedChecks; return other; }
  bool start() override { ++starts; if (startOk) current = WebClientState::running; return startOk; }
  bool restart() override { ++restarts; if (!restartOk) { if (stoppedByFailedRestart) current = WebClientState::stopped; return false; } current = WebClientState::running; return true; }
  bool waitRunning() override { if (runsAfterWait) current = WebClientState::running; return runsAfterWait; }
};
void webClient() {
  { FakeWebClient s; s.current = WebClientState::missing; const auto o = prepareWebClient(s); CHECK(o.error == "drive_webclient_unavailable" && o.stage == 7 && s.starts == 0 && s.restarts == 0); }
  { FakeWebClient s; s.current = WebClientState::disabled; const auto o = prepareWebClient(s); CHECK(o.error == "drive_webclient_unavailable" && o.stage == 7); }
  // Stopped: started once; the values written before the start apply by themselves.
  { FakeWebClient s; s.current = WebClientState::stopped; s.raised = true; const auto o = prepareWebClient(s); CHECK(o.error.empty() && o.started && !o.restarted && !o.oldLimit && o.stage == 6 && s.starts == 1 && s.restarts == 0 && s.usedChecks == 0); }
  { FakeWebClient s; s.current = WebClientState::stopped; s.startOk = false; const auto o = prepareWebClient(s); CHECK(o.error == "drive_webclient_unavailable" && o.stage == 7); }
  // Running with current caps: left alone, nothing enumerated.
  { FakeWebClient s; const auto o = prepareWebClient(s); CHECK(o.error.empty() && !o.started && !o.restarted && !o.oldLimit && o.stage == 8 && s.restarts == 0 && s.usedChecks == 0); }
  // Running, values just raised, nothing else uses WebDAV: restarted so the caps apply.
  { FakeWebClient s; s.raised = true; const auto o = prepareWebClient(s); CHECK(o.error.empty() && o.restarted && !o.oldLimit && o.stage == 9 && s.restarts == 1); }
  // Running since before the values were written (an earlier session raised them): same.
  { FakeWebClient s; s.newer = true; const auto o = prepareWebClient(s); CHECK(o.restarted && o.stage == 9 && s.restarts == 1); }
  // Someone else's WebDAV connection (SharePoint ...) or an unreadable list: never dropped; the client is told.
  { FakeWebClient s; s.raised = true; s.other = true; const auto o = prepareWebClient(s); CHECK(o.error.empty() && !o.restarted && o.oldLimit && o.stage == 10 && s.restarts == 0); }
  { FakeWebClient s; s.newer = true; s.other = true; const auto o = prepareWebClient(s); CHECK(o.oldLimit && o.stage == 10 && s.restarts == 0); }
  // Restart refused but the service still runs: map anyway, old cap reported.
  { FakeWebClient s; s.raised = true; s.restartOk = false; const auto o = prepareWebClient(s); CHECK(o.error.empty() && o.oldLimit && !o.restarted && o.stage == 11); }
  // Restart stopped it and could not start it again: unavailable, not a silent success.
  { FakeWebClient s; s.raised = true; s.restartOk = false; s.stoppedByFailedRestart = true; s.runsAfterWait = false; const auto o = prepareWebClient(s); CHECK(o.error == "drive_webclient_unavailable" && o.stage == 7); }
  // ...and one that comes back by itself (trigger start) is a running service with the old cap.
  { FakeWebClient s; s.raised = true; s.restartOk = false; s.stoppedByFailedRestart = true; const auto o = prepareWebClient(s); CHECK(o.error.empty() && o.oldLimit && o.stage == 11); }
  // Starting up: waited for, then judged like a running service.
  { FakeWebClient s; s.current = WebClientState::pending; const auto o = prepareWebClient(s); CHECK(o.error.empty() && o.stage == 8 && s.restarts == 0); }
  { FakeWebClient s; s.current = WebClientState::pending; s.raised = true; const auto o = prepareWebClient(s); CHECK(o.restarted && s.restarts == 1); }
  { FakeWebClient s; s.current = WebClientState::pending; s.runsAfterWait = false; const auto o = prepareWebClient(s); CHECK(o.error == "drive_webclient_unavailable"); }
  // The mounter reports the old cap after prepare().
  { auto system = std::make_unique<FakeWebClient>(); system->raised = true; system->other = true; WindowsMounter mounter(std::move(system)); CHECK(mounter.prepare().empty() && mounter.oldLimit()); }
  { auto system = std::make_unique<FakeWebClient>(); system->raised = true; WindowsMounter mounter(std::move(system)); CHECK(mounter.prepare().empty() && !mounter.oldLimit()); }
}

// ---- the one-time notices to the person at the PC ----
std::unique_ptr<Redirection> noticeSession(FakeLink*& link, FakeMounter*& mounter, bool notices, bool oldLimit, std::string& docsToken) {
  auto linkOwned = std::make_unique<FakeLink>(); auto mounterOwned = std::make_unique<FakeMounter>(); mounterOwned->old = oldLimit;
  link = linkOwned.get(); mounter = mounterOwned.get();
  auto redirection = std::make_unique<Redirection>(std::move(linkOwned), std::move(mounterOwned), std::chrono::milliseconds(3000));
  Credentials credentials; credentials.origin = L"https://broker.example"; credentials.token = std::string(43, 'a');
  const std::string session = "11111111-2222-3333-4444-555555555555";
  redirection->start(credentials, session, "wss://broker.example/api/v1/hosts/sessions/" + session + "/drive", 1);
  link->text("{\"type\":\"drive.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"drive-v1\"," + (notices ? "\"notices\":true," : "") + "\"limits\":{\"maxShares\":8,\"chunkBytes\":262144,\"maxInFlight\":8,\"listPage\":256,\"maxPath\":1024,\"maxName\":255,\"maxDepth\":64}}");
  link->text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false}]}");
  CHECK(link->wait([](const std::string& text) { return text.find("\"state\":\"mapped\"") != std::string::npos; }).has_value());
  docsToken = redirection->token("docs");
  return redirection;
}
size_t countNotices(FakeLink& link, const std::string& code) { std::lock_guard lock(link.mutex); size_t n = 0; for (const auto& text : link.texts) if (text == "{\"type\":\"drive.notice\",\"code\":\"" + code + "\"}") ++n; return n; }
size_t anyNotices(FakeLink& link) { std::lock_guard lock(link.mutex); size_t n = 0; for (const auto& text : link.texts) if (text.find("drive.notice") != std::string::npos) ++n; return n; }
void rawPutOfSize(uint16_t port, const std::string& token, uint64_t length) {
  SOCKET socket = connectTo(port); CHECK(socket != INVALID_SOCKET); if (socket == INVALID_SOCKET) return;
  sendText(socket, "PUT /" + token + "/huge.bin HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(length) + "\r\n\r\n");
  Sleep(300); closesocket(socket);
}
void notices() {
  FakeLink* link = nullptr; FakeMounter* mounter = nullptr; std::string docs;
  // Old WebClient cap: said once when the broker can pass it on.
  { auto session = noticeSession(link, mounter, true, true, docs);
    CHECK(eventually([&] { return countNotices(*link, "drive_webclient_old_limit") == 1; }));
    link->text("{\"type\":\"drive.shares\",\"shares\":[{\"id\":\"docs\",\"name\":\"Documents\",\"readOnly\":false},{\"id\":\"pics\",\"name\":\"Pictures\",\"readOnly\":false}]}");
    CHECK(link->wait([](const std::string& text) { return text.find("\"id\":\"pics\",\"state\":\"mapped\"") != std::string::npos; }).has_value());
    Sleep(100); CHECK(countNotices(*link, "drive_webclient_old_limit") == 1 && anyNotices(*link) == 1);
    session->stop(); }
  // An older broker (no `notices` in drive.ready) would close the pair on an unknown message: nothing is sent.
  { auto session = noticeSession(link, mounter, false, true, docs);
    Sleep(200); CHECK(anyNotices(*link) == 0 && !link->aborted);
    rawPutOfSize(session->port(), docs, 5000000000ull); CHECK(anyNotices(*link) == 0 && !link->aborted);
    session->stop(); }
  // Current caps: no notice. A file of 4 GiB or more: once per session, however often it is seen.
  { auto session = noticeSession(link, mounter, true, false, docs);
    Sleep(100); CHECK(anyNotices(*link) == 0);
    rawPutOfSize(session->port(), docs, 4294967295ull); Sleep(100); CHECK(anyNotices(*link) == 0); // exactly the redirector's maximum still fits
    rawPutOfSize(session->port(), docs, 4294967296ull);
    CHECK(eventually([&] { return countNotices(*link, "drive_file_too_large") == 1; }));
    rawPutOfSize(session->port(), docs, 9000000000ull); Sleep(100); CHECK(countNotices(*link, "drive_file_too_large") == 1 && anyNotices(*link) == 1);
    session->stop(); }
}

// A file listed with 4 GiB or more is reported by the endpoint itself (listing and HEAD).
void tooLarge() {
  MemoryBackend backend; backend.file("docs", "small.bin", "abc"); backend.file("docs", "big.bin", "x"); backend.bigSizes["big.bin"] = 5000000000ull;
  DavServer server(backend, std::chrono::milliseconds(3000)); const auto port = server.start();
  std::atomic<int> reports{0}; std::string last; std::mutex guard;
  server.onTooLarge([&](const std::string& share) { std::lock_guard lock(guard); last = share; ++reports; });
  const auto token = std::string(sharePrefix) + randomToken(); server.setShare(token, {"docs", false});
  auto response = http(port, "PROPFIND", "/" + token + "/", {}, "Depth: 1\r\n");
  CHECK(response.status == 207 && response.body.find("<D:getcontentlength>5000000000</D:getcontentlength>") != std::string::npos && reports == 1 && last == "docs");
  response = http(port, "HEAD", "/" + token + "/big.bin"); CHECK(response.status == 200 && reports == 2);
  response = http(port, "HEAD", "/" + token + "/small.bin"); CHECK(response.status == 200 && reports == 2);
  response = http(port, "PROPFIND", "/" + token + "/small.bin", {}, "Depth: 0\r\n"); CHECK(response.status == 207 && reports == 2);
  // More simultaneous connections than the old limit of 16 are served.
  std::vector<SOCKET> open; for (int i = 0; i < 40; ++i) { SOCKET s = connectTo(port); if (s != INVALID_SOCKET) open.push_back(s); }
  CHECK(open.size() == 40); response = http(port, "OPTIONS", "/" + token + "/"); CHECK(response.status == 200);
  for (const auto s : open) closesocket(s);
  server.stop();
}
}

int main() {
  WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data);
  try { names(); targets(); dav(); requests(); mounts(); webClient(); orchestrator(); redials(); tooLarge(); notices(); }
  catch (const std::exception& error) { ++failures; std::cerr << "FAIL exception: " << error.what() << "\n"; }
  std::cout << "drive-v1 host checks: " << checks << ", failures: " << failures << "\n";
  WSACleanup();
  return failures ? 1 : 0;
}
