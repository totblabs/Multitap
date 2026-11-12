#pragma once
// drive-v1 loopback WebDAV endpoint (docs/drive-redirection.md). Serves the
// subset of RFC 4918 the Windows WebDAV mini-redirector (WebClient/MRxDAV)
// uses, on 127.0.0.1 and ::1 only, for paths /Multitap-<256-bit token>/...
// Every file operation is a bounded request to a Backend (the client over the
// relay); nothing here touches the VM's own files.
#include "drive_protocol.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <condition_variable>
#include <deque>
#include <future>
#include <list>
#include <mutex>
#include <optional>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

namespace host::drive {
struct Entry { std::string name; bool dir = false; uint64_t size = 0; int64_t modified = 0, created = 0; bool readOnly = false, hasSpace = false; uint64_t free = 0, total = 0; };
// setsize and flush exist only on a session that negotiated driveFs (WinFsp backend); WebDAV never uses them.
enum class Op { stat, list, read, write, create, rename, remove, touch, setsize, flush };
struct Call { Op op = Op::stat; std::string share, path, to; uint64_t offset = 0, size = 0; uint32_t length = 0, cursor = 0; bool dir = false, truncate = false, replace = false; int64_t modified = 0; std::vector<uint8_t> data; };
struct Result { std::string error; Entry entry; std::vector<Entry> entries; std::optional<uint32_t> next; std::vector<uint8_t> data; bool ok() const { return error.empty(); } };
inline std::future<Result> readyResult(std::string error) { std::promise<Result> promise; Result result; result.error = std::move(error); promise.set_value(std::move(result)); return promise.get_future(); }
class Backend {
 public:
  virtual ~Backend() = default;
  // Never throws; a closed backend answers drive_unavailable.
  virtual std::future<Result> submit(Call call) = 0;
};

// ---- text helpers -------------------------------------------------------
inline std::string lower(std::string text) { for (auto& c : text) if (c >= 'A' && c <= 'Z') c = char(c + 32); return text; }
inline std::string trim(const std::string& text) { const auto first = text.find_first_not_of(" \t"); if (first == std::string::npos) return {}; const auto last = text.find_last_not_of(" \t"); return text.substr(first, last - first + 1); }
// Strict percent-decoding of one path segment: valid UTF-8, no encoded '/',
// '\' or NUL, then a safe file name (or the share token).
inline std::optional<std::string> decodeSegment(const std::string& raw) {
  std::string out;
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '%') {
      if (i + 2 >= raw.size() || !isxdigit(static_cast<unsigned char>(raw[i + 1])) || !isxdigit(static_cast<unsigned char>(raw[i + 2]))) return std::nullopt;
      c = char(std::stoi(raw.substr(i + 1, 2), nullptr, 16)); i += 2;
    } else if (static_cast<unsigned char>(c) < 0x21 || c == 0x7f) return std::nullopt;
    if (c == '/' || c == '\\' || c == '\0') return std::nullopt;
    out += c;
  }
  try { (void)wide(out); } catch (...) { return std::nullopt; }
  return out;
}
inline std::string encodeSegment(const std::string& text) {
  static const char hex[] = "0123456789ABCDEF"; std::string out;
  for (const unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
struct Target { bool root = false; std::string token, path; };
// origin-form "/Multitap-<token>/a/b[/]" or absolute-form on a loopback host.
inline std::optional<Target> parseTarget(std::string raw) {
  if (raw == "*") return Target{true, {}, {}};
  const auto scheme = lower(raw.substr(0, 7));
  if (scheme == "http://") {
    const auto slash = raw.find('/', 7);
    auto authority = lower(raw.substr(7, slash == std::string::npos ? std::string::npos : slash - 7));
    const auto host = authority.rfind("[::1]", 0) == 0 ? std::string("[::1]") : authority.substr(0, authority.find(':'));
    if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") return std::nullopt;
    raw = slash == std::string::npos ? "/" : raw.substr(slash);
  }
  if (raw.empty() || raw[0] != '/' || raw.find_first_of("?#") != std::string::npos || raw.size() > 12288) return std::nullopt;
  std::vector<std::string> parts; size_t start = 1;
  while (start <= raw.size()) {
    const auto end = raw.find('/', start);
    parts.push_back(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (!parts.empty() && parts.back().empty()) parts.pop_back(); // trailing slash
  Target target;
  if (parts.empty()) { target.root = true; return target; }
  std::vector<std::string> decoded;
  for (const auto& part : parts) { if (part.empty()) return std::nullopt; auto value = decodeSegment(part); if (!value) return std::nullopt; decoded.push_back(*value); }
  if (!shareToken(decoded[0])) return std::nullopt;
  target.token = decoded[0];
  for (size_t i = 1; i < decoded.size(); ++i) { if (i > 1) target.path += '/'; target.path += decoded[i]; }
  if (!safePath(target.path)) return std::nullopt;
  return target;
}
inline std::string href(const std::string& token, const std::string& path, bool collection) {
  std::string out = "/" + token;
  size_t start = 0;
  while (!path.empty() && start <= path.size()) { const auto end = path.find('/', start); out += "/" + encodeSegment(path.substr(start, end == std::string::npos ? std::string::npos : end - start)); if (end == std::string::npos) break; start = end + 1; }
  return collection ? out + "/" : out;
}
inline SYSTEMTIME systemTime(int64_t ms) {
  ULARGE_INTEGER value{}; value.QuadPart = uint64_t(std::clamp<int64_t>(ms, 0, 253402300799999LL)) * 10000ULL + 116444736000000000ULL;
  FILETIME file{value.LowPart, value.HighPart}; SYSTEMTIME time{}; FileTimeToSystemTime(&file, &time); return time;
}
inline std::string httpDate(int64_t ms) {
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}; static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const auto t = systemTime(ms); char text[40]{};
  sprintf_s(text, "%s, %02u %s %04u %02u:%02u:%02u GMT", days[t.wDayOfWeek % 7], t.wDay, months[(t.wMonth + 11) % 12], t.wYear, t.wHour, t.wMinute, t.wSecond);
  return text;
}
inline std::string isoDate(int64_t ms) { const auto t = systemTime(ms); char text[32]{}; sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return text; }
inline std::optional<int64_t> parseHttpDate(const std::string& text) {
  if (text.empty() || text.size() > 64) return std::nullopt;
  SYSTEMTIME time{}; std::wstring value; try { value = wide(text); } catch (...) { return std::nullopt; }
  if (!WinHttpTimeToSystemTime(value.c_str(), &time)) return std::nullopt;
  FILETIME file{}; if (!SystemTimeToFileTime(&time, &file)) return std::nullopt;
  const uint64_t ticks = (uint64_t(file.dwHighDateTime) << 32) | file.dwLowDateTime;
  if (ticks < 116444736000000000ULL) return std::nullopt;
  return int64_t((ticks - 116444736000000000ULL) / 10000ULL);
}
inline std::string attributesHex(const Entry& entry, bool shareReadOnly) {
  DWORD value = entry.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
  if (!entry.dir && (entry.readOnly || shareReadOnly)) value |= FILE_ATTRIBUTE_READONLY;
  char text[16]{}; sprintf_s(text, "%08lX", value); return text;
}
inline std::string etag(const Entry& entry) { char text[48]{}; sprintf_s(text, "\"%llx-%llx\"", static_cast<unsigned long long>(entry.size), static_cast<unsigned long long>(entry.modified)); return text; }
inline std::string propResponse(const std::string& location, const Entry& entry, bool shareReadOnly) {
  std::string out = "<D:response><D:href>" + location + "</D:href><D:propstat><D:prop><D:resourcetype>" + (entry.dir ? "<D:collection/>" : "") + "</D:resourcetype>";
  if (!entry.dir) out += "<D:getcontentlength>" + std::to_string(entry.size) + "</D:getcontentlength><D:getetag>" + etag(entry) + "</D:getetag><D:getcontenttype>application/octet-stream</D:getcontenttype>";
  out += "<D:getlastmodified>" + httpDate(entry.modified) + "</D:getlastmodified><D:creationdate>" + isoDate(entry.created) + "</D:creationdate><Z:Win32FileAttributes>" + attributesHex(entry, shareReadOnly) + "</Z:Win32FileAttributes>";
  if (entry.hasSpace) out += "<D:quota-available-bytes>" + std::to_string(entry.free) + "</D:quota-available-bytes><D:quota-used-bytes>" + std::to_string(entry.total - std::min(entry.free, entry.total)) + "</D:quota-used-bytes>";
  return out + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
}
inline const char* statusText(int status) {
  switch (status) {
    case 100: return "Continue"; case 200: return "OK"; case 201: return "Created"; case 204: return "No Content"; case 206: return "Partial Content"; case 207: return "Multi-Status";
    case 400: return "Bad Request"; case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 409: return "Conflict"; case 411: return "Length Required";
    case 412: return "Precondition Failed"; case 413: return "Payload Too Large"; case 415: return "Unsupported Media Type"; case 416: return "Range Not Satisfiable"; case 423: return "Locked"; case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented"; case 502: return "Bad Gateway"; case 503: return "Service Unavailable"; case 504: return "Gateway Timeout"; case 507: return "Insufficient Storage";
    default: return "Error";
  }
}

struct DavShare { std::string id; bool readOnly = false; };

class DavServer {
 public:
  static constexpr size_t maxConnections = 64, maxHeaderBytes = 16384, maxSmallBody = 65536, maxListEntries = 20000, maxListXml = 9500000, pipeline = 4;
  explicit DavServer(Backend& backend, std::chrono::milliseconds timeout = std::chrono::seconds(65)) : backend_(backend), timeout_(timeout) {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("drive_server_failed");
  }
  ~DavServer() { stop(); WSACleanup(); }
  DavServer(const DavServer&) = delete; DavServer& operator=(const DavServer&) = delete;
  // Binds 127.0.0.1 and ::1 on one free port (IPv6 is optional).
  uint16_t start() {
    std::lock_guard lifecycle(lifecycle_);
    if (port_) return port_;
    for (int attempt = 0; attempt < 16 && !port_; ++attempt) {
      SOCKET v4 = listener(AF_INET, 0); if (v4 == INVALID_SOCKET) continue;
      sockaddr_in bound{}; int size = sizeof(bound); getsockname(v4, reinterpret_cast<sockaddr*>(&bound), &size);
      const uint16_t port = ntohs(bound.sin_port);
      int failure = 0; SOCKET v6 = listener(AF_INET6, port, &failure);
      if (v6 == INVALID_SOCKET && failure == WSAEADDRINUSE) { closesocket(v4); continue; }
      listeners_ = {v4, v6}; port_ = port;
    }
    if (!port_) throw std::runtime_error("drive_server_failed");
    stopped_ = false;
    accept_ = std::thread([this] { acceptLoop(); });
    return port_;
  }
  uint16_t port() const { return port_; }
  void stop() {
    std::lock_guard lifecycle(lifecycle_);
    stopped_ = true;
    if (accept_.joinable()) accept_.join();
    for (auto& socket : listeners_) if (socket != INVALID_SOCKET) { closesocket(socket); socket = INVALID_SOCKET; }
    std::list<Worker> threads;
    { std::lock_guard lock(mutex_); for (const auto socket : active_) { shutdown(socket, SD_BOTH); } threads.swap(threads_); }
    for (auto& worker : threads) if (worker.thread.joinable()) worker.thread.join();
    port_ = 0; clearShares();
  }
  // MRxDAV cannot open a file of 4 GiB or more (its size cap is a DWORD). The
  // first sight of one on a share (a listing, a read, a write) is reported here;
  // the request itself is answered as usual and fails in Windows.
  static constexpr uint64_t redirectorFileLimit = 0xffffffffull;
  using TooLarge = std::function<void(const std::string& shareId)>;
  void onTooLarge(TooLarge value) { std::lock_guard lock(mutex_); tooLarge_ = std::move(value); }
  void setShare(const std::string& token, DavShare share) { std::lock_guard lock(mutex_); shares_[token] = std::move(share); }
  void removeShare(const std::string& token) { std::lock_guard lock(mutex_); shares_.erase(token); }
  void clearShares() { std::lock_guard lock(mutex_); shares_.clear(); }

 private:
  Backend& backend_; std::chrono::milliseconds timeout_;
  std::recursive_mutex lifecycle_; std::mutex mutex_; std::atomic<bool> stopped_{true};
  std::array<SOCKET, 2> listeners_{INVALID_SOCKET, INVALID_SOCKET}; uint16_t port_ = 0; std::thread accept_;
  struct Worker { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
  TooLarge tooLarge_;
  void reportTooLarge(const DavShare& share) { TooLarge callback; { std::lock_guard lock(mutex_); callback = tooLarge_; } if (callback) callback(share.id); }
  std::list<Worker> threads_; std::set<SOCKET> active_; std::map<std::string, DavShare> shares_;

  static SOCKET listener(int family, uint16_t port, int* failure = nullptr) {
    SOCKET socket = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) { if (failure) *failure = WSAGetLastError(); return INVALID_SOCKET; }
    BOOL on = TRUE; setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    int result = 0;
    if (family == AF_INET) { sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); result = bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)); }
    else { setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on)); sockaddr_in6 address{}; address.sin6_family = AF_INET6; address.sin6_port = htons(port); address.sin6_addr = in6addr_loopback; result = bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)); }
    if (result || listen(socket, SOMAXCONN)) { if (failure) *failure = WSAGetLastError(); closesocket(socket); return INVALID_SOCKET; }
    return socket;
  }
  void acceptLoop() {
    while (!stopped_) {
      std::array<WSAPOLLFD, 2> polls{}; ULONG count = 0;
      for (const auto socket : listeners_) if (socket != INVALID_SOCKET) { polls[count].fd = socket; polls[count].events = POLLRDNORM; ++count; }
      if (!count || WSAPoll(polls.data(), count, 200) <= 0) continue;
      for (ULONG i = 0; i < count; ++i) {
        if (!(polls[i].revents & POLLRDNORM)) continue;
        SOCKET client = accept(polls[i].fd, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::lock_guard lock(mutex_);
        // Finished connection threads are joined here.
        for (auto it = threads_.begin(); it != threads_.end();) { if (*it->done) { it->thread.join(); it = threads_.erase(it); } else ++it; }
        if (stopped_ || active_.size() >= maxConnections) { closesocket(client); continue; }
        active_.insert(client);
        auto done = std::make_shared<std::atomic<bool>>(false);
        threads_.push_back({std::thread([this, client, done] { serve(client); *done = true; }), done});
      }
    }
  }
  // ---- connection ----
  struct Reader {
    SOCKET socket; std::string buffer; size_t at = 0;
    bool fill() { if (at && at == buffer.size()) { buffer.clear(); at = 0; } else if (at > 65536) { buffer.erase(0, at); at = 0; } char data[65536]; const int got = recv(socket, data, sizeof(data), 0); if (got <= 0) return false; buffer.append(data, size_t(got)); return true; }
    std::optional<std::string> line(size_t max) { for (;;) { const auto end = buffer.find("\r\n", at); if (end != std::string::npos) { if (end - at > max) return std::nullopt; std::string out = buffer.substr(at, end - at); at = end + 2; return out; } if (buffer.size() - at > max || !fill()) return std::nullopt; } }
    bool read(size_t count, std::string& out) { while (buffer.size() - at < count) if (!fill()) return false; out.assign(buffer, at, count); at += count; return true; }
  };
  struct Request { std::string method, target, version; std::map<std::string, std::string> headers; bool chunked = false; std::optional<uint64_t> length; bool keepAlive = true;
    std::string header(const std::string& name) const { const auto found = headers.find(name); return found == headers.end() ? std::string() : found->second; } };
  // 0: closed/invalid, 1: request, else an HTTP status to answer and close.
  static int readRequest(Reader& reader, Request& request) {
    std::optional<std::string> line;
    do { line = reader.line(8192); if (!line) return 0; } while (line->empty());
    const auto first = line->find(' '), second = line->find(' ', first == std::string::npos ? first : first + 1);
    if (first == std::string::npos || second == std::string::npos || line->find(' ', second + 1) != std::string::npos) return 400;
    request.method = line->substr(0, first); request.target = line->substr(first + 1, second - first - 1); request.version = line->substr(second + 1);
    if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") return 400;
    size_t total = line->size();
    for (;;) {
      auto header = reader.line(maxHeaderBytes); if (!header) return 431;
      if (header->empty()) break;
      total += header->size(); if (total > maxHeaderBytes || request.headers.size() >= 100) return 431;
      const auto colon = header->find(':'); if (colon == std::string::npos || colon == 0) return 400;
      const auto name = lower(header->substr(0, colon)), value = trim(header->substr(colon + 1));
      if (name.find_first_of(" \t") != std::string::npos) return 400;
      if (request.headers.count(name)) { if ((name == "content-length" || name == "transfer-encoding" || name == "destination") && request.headers[name] != value) return 400; continue; }
      request.headers[name] = value;
    }
    const auto encoding = lower(request.header("transfer-encoding")), length = request.header("content-length");
    if (!encoding.empty()) { if (encoding != "chunked" || !length.empty()) return encoding == "chunked" ? 400 : 501; request.chunked = true; }
    if (!length.empty()) { if (length.size() > 15 || length.find_first_not_of("0123456789") != std::string::npos) return 400; request.length = std::stoull(length); }
    const auto connection = lower(request.header("connection"));
    request.keepAlive = request.version == "HTTP/1.1" ? connection.find("close") == std::string::npos : connection.find("keep-alive") != std::string::npos;
    return 1;
  }
  // Reads a request body for PUT in pieces; small bodies whole.
  struct Body {
    Reader& reader; const Request& request; uint64_t remaining = 0; bool done = false; uint64_t chunkLeft = 0;
    Body(Reader& r, const Request& q) : reader(r), request(q) { if (!q.chunked) { remaining = q.length.value_or(0); done = remaining == 0; } }
    // Appends up to `max` bytes; false on a malformed or lost body.
    bool next(size_t max, std::string& out) {
      out.clear();
      while (!done && out.size() < max) {
        if (!request.chunked) { const size_t count = size_t(std::min<uint64_t>(remaining, max - out.size())); std::string piece; if (!reader.read(count, piece)) return false; out += piece; remaining -= count; done = remaining == 0; continue; }
        if (!chunkLeft) {
          auto size = reader.line(64); if (!size) return false;
          const auto text = trim(size->substr(0, size->find(';')));
          if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
          chunkLeft = std::stoull(text, nullptr, 16);
          if (!chunkLeft) { for (;;) { auto trailer = reader.line(1024); if (!trailer) return false; if (trailer->empty()) break; } done = true; break; }
        }
        const size_t count = size_t(std::min<uint64_t>(chunkLeft, max - out.size())); std::string piece; if (!reader.read(count, piece)) return false; out += piece; chunkLeft -= count;
        if (!chunkLeft) { std::string crlf; if (!reader.read(2, crlf) || crlf != "\r\n") return false; }
      }
      return true;
    }
    bool all(size_t limit, std::string& out) {
      out.clear(); std::string piece;
      while (!done) { if (!next(65536, piece)) return false; out += piece; if (out.size() > limit) return false; }
      return true;
    }
  };
  static bool sendAll(SOCKET socket, const char* data, size_t size) { while (size) { const int sent = send(socket, data, int(std::min<size_t>(size, 1 << 20)), 0); if (sent <= 0) return false; data += sent; size -= size_t(sent); } return true; }
  static bool sendAll(SOCKET socket, const std::string& data) { return sendAll(socket, data.data(), data.size()); }
  static std::string head(int status, const std::vector<std::pair<std::string, std::string>>& headers, bool keepAlive) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\nServer: Multitap-Host\r\n";
    for (const auto& [name, value] : headers) out += name + ": " + value + "\r\n";
    return out + (keepAlive ? "" : "Connection: close\r\n") + "\r\n";
  }
  static bool respond(SOCKET socket, int status, bool keepAlive, std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = {}, bool headOnly = false) {
    headers.emplace_back("Content-Length", std::to_string(body.size()));
    return sendAll(socket, head(status, headers, keepAlive) + (headOnly ? std::string() : body)) && keepAlive;
  }
  Result wait(std::future<Result>& future) {
    if (future.wait_for(timeout_) != std::future_status::ready) { Result result; result.error = "drive_timeout"; return result; }
    try { return future.get(); } catch (...) { Result result; result.error = "drive_unavailable"; return result; }
  }
  Result call(Call value) { auto future = backend_.submit(std::move(value)); return wait(future); }
  void serve(SOCKET socket) {
    DWORD timeout = 120000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)); setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    BOOL on = TRUE; setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    Reader reader{socket};
    try {
      while (!stopped_) {
        Request request; const int parsed = readRequest(reader, request);
        if (!parsed) break;
        if (parsed != 1) { respond(socket, parsed, false); break; }
        if (!handle(socket, reader, request) || stopped_) break;
      }
    } catch (...) { }
    // Lingering close: an answer sent before the whole request was read must
    // not be lost to a reset, so unread input is drained briefly first.
    shutdown(socket, SD_SEND);
    DWORD linger = 1000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&linger), sizeof(linger));
    char drain[16384]; size_t drained = 0; int got = 0;
    while (!stopped_ && drained < (4u << 20) && (got = recv(socket, drain, sizeof(drain), 0)) > 0) drained += size_t(got);
    { std::lock_guard lock(mutex_); active_.erase(socket); }
    closesocket(socket);
  }
  std::optional<DavShare> share(const std::string& token) { std::lock_guard lock(mutex_); const auto found = shares_.find(token); if (found == shares_.end()) return std::nullopt; return found->second; }
  static std::string multistatus(const std::string& responses) { return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<D:multistatus xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\">" + responses + "</D:multistatus>"; }
  // Handles one request; false closes the connection.
  bool handle(SOCKET socket, Reader& reader, const Request& request) {
    const auto& method = request.method; const bool keep = request.keepAlive;
    Body body(reader, request);
    const auto smallBody = [&](std::string& out) { return body.all(maxSmallBody, out); };
    if (method == "OPTIONS") {
      std::string ignored; if (!smallBody(ignored)) return respond(socket, 413, false);
      return respond(socket, 200, keep, {{"DAV", "1, 2"}, {"MS-Author-Via", "DAV"}, {"Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, LOCK, UNLOCK"}});
    }
    const auto target = parseTarget(request.target);
    if (!target) return respond(socket, 400, false);
    if (target->root) {
      std::string ignored; if (!smallBody(ignored)) return respond(socket, 413, false);
      if (method != "PROPFIND") return respond(socket, method == "GET" || method == "HEAD" ? 404 : 403, keep);
      Entry root; root.dir = true;
      return respond(socket, 207, keep, {{"Content-Type", "text/xml; charset=\"utf-8\""}}, multistatus(propResponse("/", root, true)));
    }
    const auto found = share(target->token);
    if (!found) { std::string ignored; const bool drained = method != "PUT" && smallBody(ignored); return respond(socket, 404, keep && drained); }
    const auto modifying = method == "PUT" || method == "DELETE" || method == "MKCOL" || method == "MOVE" || method == "PROPPATCH" || method == "LOCK";
    if (modifying && found->readOnly) { std::string ignored; const bool drained = method != "PUT" && smallBody(ignored); return respond(socket, 403, keep && drained); }
    const auto base = [&](Op op) { Call value; value.op = op; value.share = found->id; value.path = target->path; return value; };
    const auto self = [&](bool collection) { return href(target->token, target->path, collection); };
    if (method == "PROPFIND") return propfind(socket, request, body, *target, *found, keep);
    if (method == "GET" || method == "HEAD") { std::string ignored; if (!smallBody(ignored)) return respond(socket, 413, false); return get(socket, request, *target, *found, keep, method == "HEAD"); }
    if (method == "PUT") return put(socket, request, body, *target, *found, keep);
    if (method == "DELETE") {
      std::string ignored; if (!smallBody(ignored)) return respond(socket, 413, false);
      if (target->path.empty()) return respond(socket, 403, keep);
      const auto result = call(base(Op::remove)); return result.ok() ? respond(socket, 204, keep) : respond(socket, httpStatus(result.error), keep);
    }
    if (method == "MKCOL") {
      std::string content; if (!smallBody(content)) return respond(socket, 413, false);
      if (!content.empty()) return respond(socket, 415, keep);
      if (target->path.empty()) return respond(socket, 405, keep);
      auto value = base(Op::create); value.dir = true;
      const auto result = call(std::move(value));
      if (result.ok()) return respond(socket, 201, keep);
      return respond(socket, result.error == "drive_exists" ? 405 : result.error == "drive_not_found" ? 409 : httpStatus(result.error), keep);
    }
    if (method == "MOVE") {
      std::string ignored; if (!smallBody(ignored)) return respond(socket, 413, false);
      const auto destination = parseTarget(request.header("destination"));
      if (!destination || destination->root) return respond(socket, 400, keep);
      if (destination->token != target->token) return respond(socket, 502, keep);
      if (target->path.empty() || destination->path.empty() || destination->path == target->path) return respond(socket, 403, keep);
      const auto overwrite = request.header("overwrite");
      if (!overwrite.empty() && overwrite != "T" && overwrite != "F") return respond(socket, 400, keep);
      // "report.txt" -> "Report.txt" (Explorer's rename, with Overwrite: F): the
      // destination "exists" because it is the same file under another
      // capitalisation. That is a rename, not an overwrite; the PC checks that
      // the two really are one object.
      const auto sameFile = [&] { const auto a = wide(target->path), b = wide(destination->path); return a.size() == b.size() && CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL; }();
      Call existing = base(Op::stat); existing.path = destination->path;
      const auto before = sameFile ? Result{} : call(std::move(existing));
      if (!sameFile && !before.ok() && before.error != "drive_not_found") return respond(socket, httpStatus(before.error), keep);
      if (!sameFile && before.ok() && overwrite == "F") return respond(socket, 412, keep);
      auto value = base(Op::rename); value.to = destination->path; value.replace = overwrite != "F";
      const auto result = call(std::move(value));
      if (!result.ok()) return respond(socket, result.error == "drive_exists" ? 412 : httpStatus(result.error), keep);
      return respond(socket, before.ok() && !sameFile ? 204 : 201, keep);
    }
    if (method == "COPY") { std::string ignored; const bool drained = smallBody(ignored); return respond(socket, 501, keep && drained); }
    if (method == "LOCK") {
      std::string content; if (!smallBody(content)) return respond(socket, 413, false);
      int status = 200;
      if (!content.empty() || request.header("if").empty()) {
        const auto existing = call(base(Op::stat));
        if (!existing.ok()) {
          if (existing.error != "drive_not_found" || target->path.empty()) return respond(socket, httpStatus(existing.error), keep);
          // A lock on a missing file creates it (MRxDAV's new-file sequence).
          auto create = base(Op::create); const auto created = call(std::move(create));
          if (!created.ok() && created.error != "drive_exists") return respond(socket, httpStatus(created.error), keep);
          status = created.ok() ? 201 : 200;
        }
      }
      GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, 40); std::string guid = utf8(text); guid = guid.substr(1, guid.size() - 2);
      const auto token = "opaquelocktoken:" + lower(guid);
      const auto xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock><D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>0</D:depth><D:timeout>Second-3600</D:timeout><D:locktoken><D:href>" + token + "</D:href></D:locktoken><D:lockroot><D:href>" + self(false) + "</D:href></D:lockroot></D:activelock></D:lockdiscovery></D:prop>";
      return respond(socket, status, keep, {{"Lock-Token", "<" + token + ">"}, {"Content-Type", "text/xml; charset=\"utf-8\""}}, xml);
    }
    if (method == "UNLOCK") { std::string ignored; if (!smallBody(ignored)) return respond(socket, 413, false); return respond(socket, 204, keep); }
    if (method == "PROPPATCH") {
      std::string content; if (!smallBody(content)) return respond(socket, 413, false);
      std::string props;
      for (const auto* name : {"Win32CreationTime", "Win32LastAccessTime", "Win32LastModifiedTime", "Win32FileAttributes"})
        if (content.find(std::string(name) + ">") != std::string::npos || content.find(std::string(name) + "/>") != std::string::npos) props += "<Z:" + std::string(name) + "/>";
      // The last-modified time is applied; creation/access times and attributes are acknowledged.
      const std::string marker = "Win32LastModifiedTime>"; const auto at = content.find(marker);
      if (at != std::string::npos && !target->path.empty()) {
        const auto end = content.find('<', at + marker.size());
        if (const auto time = parseHttpDate(trim(content.substr(at + marker.size(), end == std::string::npos ? std::string::npos : end - at - marker.size())))) {
          auto value = base(Op::touch); value.modified = *time;
          const auto result = call(std::move(value));
          if (!result.ok() && result.error != "drive_is_directory") return respond(socket, httpStatus(result.error), keep);
        }
      }
      const auto xml = multistatus("<D:response><D:href>" + self(false) + "</D:href><D:propstat><D:prop>" + props + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
      return respond(socket, 207, keep, {{"Content-Type", "text/xml; charset=\"utf-8\""}}, xml);
    }
    std::string ignored; const bool drained = smallBody(ignored);
    return respond(socket, 405, keep && drained, {{"Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, LOCK, UNLOCK"}});
  }
  bool propfind(SOCKET socket, const Request& request, Body& body, const Target& target, const DavShare& share, bool keep) {
    std::string ignored; if (!body.all(maxSmallBody, ignored)) return respond(socket, 413, false);
    const auto depth = lower(request.header("depth"));
    if (!depth.empty() && depth != "0" && depth != "1" && depth != "infinity") return respond(socket, 400, keep);
    Call stat; stat.op = Op::stat; stat.share = share.id; stat.path = target.path;
    auto self = call(std::move(stat));
    if (!self.ok()) return respond(socket, httpStatus(self.error), keep);
    if (!target.path.empty()) self.entry.hasSpace = false;
    // Streamed (chunked) so a large folder is never buffered whole.
    if (!sendAll(socket, head(207, {{"Content-Type", "text/xml; charset=\"utf-8\""}, {"Transfer-Encoding", "chunked"}}, keep))) return false;
    const auto chunk = [&](const std::string& data) { if (data.empty()) return true; char size[16]{}; sprintf_s(size, "%zx\r\n", data.size()); return sendAll(socket, std::string(size) + data + "\r\n"); };
    std::string document = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<D:multistatus xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\">" + propResponse(href(target.token, target.path, self.entry.dir), self.entry, share.readOnly);
    size_t sent = document.size(), entries = 0;
    if (!chunk(document)) return false;
    if (self.entry.dir && depth != "0") {
      uint32_t cursor = 0;
      for (;;) {
        Call list; list.op = Op::list; list.share = share.id; list.path = target.path; list.cursor = cursor;
        const auto page = call(std::move(list));
        if (!page.ok()) break; // a truncated listing is still a valid document
        std::string xml;
        for (const auto& item : page.entries) {
          if (entries >= maxListEntries || sent + xml.size() > maxListXml) break;
          if (item.size > redirectorFileLimit) reportTooLarge(share);
          const auto child = target.path.empty() ? item.name : target.path + "/" + item.name;
          xml += propResponse(href(target.token, child, item.dir), item, share.readOnly); ++entries;
        }
        sent += xml.size(); if (!chunk(xml)) return false;
        if (!page.next || *page.next <= cursor || entries >= maxListEntries || sent > maxListXml) break;
        cursor = *page.next;
      }
    }
    return chunk("</D:multistatus>") && sendAll(socket, "0\r\n\r\n") && keep;
  }
  bool get(SOCKET socket, const Request& request, const Target& target, const DavShare& share, bool keep, bool headOnly) {
    Call stat; stat.op = Op::stat; stat.share = share.id; stat.path = target.path;
    const auto info = call(std::move(stat));
    if (!info.ok()) return respond(socket, httpStatus(info.error), keep);
    if (info.entry.dir) return respond(socket, 403, keep);
    const uint64_t size = info.entry.size; uint64_t first = 0, count = size; bool partial = false;
    if (size > redirectorFileLimit) reportTooLarge(share);
    const auto range = lower(request.header("range"));
    if (!range.empty() && range.rfind("bytes=", 0) == 0 && range.find(',') == std::string::npos) {
      const auto spec = range.substr(6); const auto dash = spec.find('-');
      const auto digits = [](const std::string& text) { return !text.empty() && text.size() <= 18 && text.find_first_not_of("0123456789") == std::string::npos; };
      if (dash != std::string::npos) {
        const auto from = spec.substr(0, dash), to = spec.substr(dash + 1);
        if (digits(from) && (to.empty() || digits(to))) {
          first = std::stoull(from); const uint64_t last = to.empty() ? (size ? size - 1 : 0) : std::min<uint64_t>(std::stoull(to), size ? size - 1 : 0);
          if (first >= size || (!to.empty() && std::stoull(to) < first)) return respond(socket, 416, keep, {{"Content-Range", "bytes */" + std::to_string(size)}});
          count = last - first + 1; partial = true;
        } else if (from.empty() && digits(to)) {
          const uint64_t suffix = std::min<uint64_t>(std::stoull(to), size);
          if (!suffix) return respond(socket, 416, keep, {{"Content-Range", "bytes */" + std::to_string(size)}});
          first = size - suffix; count = suffix; partial = true;
        }
      }
    }
    std::vector<std::pair<std::string, std::string>> headers{{"Content-Type", "application/octet-stream"}, {"Last-Modified", httpDate(info.entry.modified)}, {"ETag", etag(info.entry)}, {"Accept-Ranges", "bytes"}, {"Content-Length", std::to_string(count)}};
    if (partial) headers.emplace_back("Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(first + count - 1) + "/" + std::to_string(size));
    if (!sendAll(socket, head(partial ? 206 : 200, headers, keep))) return false;
    if (headOnly) return keep;
    std::deque<std::pair<uint64_t, std::future<Result>>> reads; uint64_t next = first, at = first; const uint64_t end = first + count;
    while (at < end) {
      while (reads.size() < pipeline && next < end) {
        Call read; read.op = Op::read; read.share = share.id; read.path = target.path; read.offset = next; read.length = uint32_t(std::min<uint64_t>(chunkBytes, end - next));
        reads.emplace_back(read.length, backend_.submit(std::move(read))); next += reads.back().first;
      }
      auto [expected, future] = std::move(reads.front()); reads.pop_front();
      const auto result = wait(future);
      // A failed or short read after the headers cannot be reported: the file
      // changed or the channel ended, so the connection is cut.
      if (!result.ok() || result.data.size() != expected) return false;
      if (!sendAll(socket, reinterpret_cast<const char*>(result.data.data()), result.data.size())) return false;
      at += expected;
    }
    return keep;
  }
  bool put(SOCKET socket, const Request& request, Body& body, const Target& target, const DavShare& share, bool keep) {
    if (target.path.empty()) return respond(socket, 405, false);
    if (!request.chunked && !request.length) return respond(socket, 411, false);
    if (request.length && *request.length > (1ULL << 40)) return respond(socket, 413, false);
    if (request.length && *request.length > redirectorFileLimit) reportTooLarge(share);
    Call stat; stat.op = Op::stat; stat.share = share.id; stat.path = target.path;
    const auto before = call(std::move(stat));
    if (!before.ok() && before.error != "drive_not_found") return respond(socket, httpStatus(before.error), false);
    if (before.ok() && before.entry.dir) return respond(socket, 405, false);
    if (lower(request.header("expect")) == "100-continue" && !sendAll(socket, "HTTP/1.1 100 Continue\r\n\r\n")) return false;
    std::string piece;
    if (!body.next(chunkBytes, piece)) return false;
    if (piece.empty()) {
      Call create; create.op = Op::create; create.share = share.id; create.path = target.path; create.truncate = true;
      const auto result = call(std::move(create));
      return result.ok() ? respond(socket, before.ok() ? 204 : 201, keep) : respond(socket, httpStatus(result.error), false);
    }
    const auto write = [&](uint64_t offset, bool truncate, std::string&& data) { Call value; value.op = Op::write; value.share = share.id; value.path = target.path; value.offset = offset; value.truncate = truncate; value.data.assign(data.begin(), data.end()); return backend_.submit(std::move(value)); };
    // The first write creates/empties the file and must land before the rest.
    uint64_t offset = piece.size();
    { auto first = write(0, true, std::move(piece)); const auto result = wait(first); if (!result.ok()) return respond(socket, httpStatus(result.error), false); }
    std::deque<std::future<Result>> writes;
    const auto settle = [&]() -> std::string { auto future = std::move(writes.front()); writes.pop_front(); const auto result = wait(future); return result.error; };
    for (;;) {
      if (!body.next(chunkBytes, piece)) return false;
      if (piece.empty()) break;
      const uint64_t size = piece.size();
      if (offset <= redirectorFileLimit && offset + size > redirectorFileLimit) reportTooLarge(share);
      writes.push_back(write(offset, false, std::move(piece))); offset += size;
      if (writes.size() >= pipeline) { const auto error = settle(); if (!error.empty()) return respond(socket, httpStatus(error), false); }
    }
    while (!writes.empty()) { const auto error = settle(); if (!error.empty()) return respond(socket, httpStatus(error), false); }
    return respond(socket, before.ok() ? 204 : 201, keep);
  }
};
}
