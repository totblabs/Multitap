// Host side of the clipboard end-to-end harness: the real ClipboardWorker (apps/windows_host/src/clipboard_worker.hpp)
// connected to a relay over a plain loopback WebSocket with the headers of ClipboardChannel, and an in-memory stand-in
// for the VM's clipboard. Driven by JSON lines on stdin (see README.md); events on stdout.
#include "e2e_common.hpp"
#include "clipboard_worker.hpp"
#include "websocket_send.hpp"

using namespace host;
namespace {
using e2e::JV;
using e2e::object;

bool scaleHeader = true;     // the host announces the scale revision unless started with --no-scale
bool onPasteHeader = true;   // and the on-paste revision unless started with --no-onpaste
bool dropWaiting = false;    // --drop-waiting: a Host that never reports a waiting paste (the event is optional for the card)
int emitDelayMs = 0;         // --emit-delay-ms N: every data chunk this Host sends is held back N ms (a slow link from the VM to the PC)
bool pullQueueHeader = true; // the pull queue (pull-queue-4-v1) unless started with --no-pullqueue
bool deferredHeader = true;  // the deferred manifest (manifest-on-paste-v1) unless started with --no-deferred
bool quiet = false;          // --quiet (throughput benchmark): no event line per data message; the service's JSON parse of it still runs

// ClipboardChannel (apps/windows_host/src/clipboard_channel.hpp) for a loopback ws:// relay: the same headers, queue bounds and
// message limits; only the TLS flag and the origin check differ.
class PlainChannel {
 public:
  using Receive = std::function<void(const std::string&)>;
  ~PlainChannel() { stop(); }
  void stop() { close(); if (thread_.joinable()) thread_.join(); }
  void send(const std::string& json) {
    bool overflow = false;
    { std::lock_guard lock(mutex_); if (stopped_) return; if (json.size() > 65000 || queue_.size() >= 192 || bytes_ + json.size() > 12 * 1024 * 1024) overflow = true; else { queue_.push_back(json); bytes_ += json.size(); } }
    if (overflow) close(); else condition_.notify_one();
  }
  void start(const std::string& url, const std::string& token, Receive receive) {
    receive_ = std::move(receive); stopped_ = false;
    thread_ = std::thread([this, url, token] {
      std::thread sender;
      try {
        const auto wideUrl = wide("http" + url.substr(2));
        URL_COMPONENTS parts{};  // (WinHttpCrackUrl does not know ws://)
        parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = DWORD(-1); parts.dwUrlPathLength = DWORD(-1);
        if (!WinHttpCrackUrl(wideUrl.c_str(), DWORD(wideUrl.size()), 0, &parts)) throw std::runtime_error("bad url");
        const std::wstring hostName(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
        Internet session(WinHttpOpen(L"MultitapHostE2E", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        Internet connection(WinHttpConnect(session.handle, hostName.c_str(), parts.nPort, 0));
        Internet request(WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0), "WebSocket");
        const auto authorization = L"Authorization: Host " + wide(token) + L"\r\nX-Multitap-Clipboard-Limits: 2\r\nX-Multitap-Clipboard-Formats: 2\r\nX-Multitap-Clipboard-Pull: 1\r\n" + std::wstring(scaleHeader ? L"X-Multitap-Clipboard-Scale: 1\r\n" : L"") + std::wstring(onPasteHeader ? L"X-Multitap-Clipboard-OnPaste: 1\r\n" : L"") + std::wstring(pullQueueHeader ? L"X-Multitap-Clipboard-PullQueue: 1\r\n" : L"") + std::wstring(deferredHeader ? L"X-Multitap-Clipboard-Deferred: 1\r\n" : L"");
        winCheck(WinHttpSendRequest(request.handle, authorization.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "connect");
        winCheck(WinHttpReceiveResponse(request.handle, nullptr), "response");
        DWORD status = 0, length = sizeof(status);
        winCheck(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX), "status");
        if (status != 101) throw std::runtime_error("relay refused the clipboard channel: " + std::to_string(status));
        const auto web = WinHttpWebSocketCompleteUpgrade(request.handle, 0); if (!web) winCheck(FALSE, "upgrade");
        socket_ = web;
        sender = std::thread([this] {
          // (The sent count for the worker's flow control, as ClipboardChannel reports it.)
          uint64_t written = 0;
          try { while (!stopped_) { std::string json; bool drained = false; { std::unique_lock lock(mutex_); condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); }); if (stopped_) break; json = std::move(queue_.front()); queue_.pop_front(); bytes_ -= json.size(); drained = queue_.empty(); } if (sendWebSocketMessageOn(socket_.load(), true, json.data(), json.size()) != NO_ERROR) break;
              if ((++written % 16 == 0 || drained) && flow_) try { flow_(written); } catch (...) {} } } catch (...) {}
          close();
        });
        std::array<char, 16384> buffer{}; std::string json;
        while (!stopped_) {
          DWORD count = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{}; const auto socket = socket_.load();
          if (!socket || WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &count, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
          if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) throw std::runtime_error("Non-JSON clipboard relay payload");
          if (json.size() + count > 65000) throw std::runtime_error("Clipboard relay message exceeds private IPC bound");
          json.append(buffer.data(), count);
          if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            const auto parsed = JsonParser::parse(json);
            if (parsed.get("type").string().rfind("clipboard.", 0) != 0) throw std::runtime_error("Unexpected clipboard relay message");
            if (receive_) receive_(json);
            json.clear();
          }
        }
      } catch (const std::exception& failure) { e2e::emitLine(object({{"event", "channelError"}, {"what", failure.what()}})); } catch (...) {}
      close(); if (sender.joinable()) sender.join();
      if (receive_) try { receive_("{\"type\":\"clipboard.channel.closed\"}"); } catch (...) {}
    });
  }
  std::function<void(uint64_t)> flow_;  // set before start: the sent count goes to the worker directly (no event line)
 private:
  std::atomic<bool> stopped_{true}; std::atomic<HINTERNET> socket_{nullptr}; std::thread thread_; Receive receive_; std::mutex mutex_; std::condition_variable condition_; std::deque<std::string> queue_; size_t bytes_ = 0;
  void close() { stopped_ = true; if (const auto socket = socket_.exchange(nullptr)) WinHttpCloseHandle(socket); { std::lock_guard lock(mutex_); queue_.clear(); bytes_ = 0; } condition_.notify_all(); }
};

e2e::Board board;
std::string argument(int argc, wchar_t** argv, const wchar_t* name) {
  for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], name)) return e2e::utf8(argv[i + 1]);
  throw std::runtime_error(std::string("missing ") + e2e::utf8(name));
}
void respond(int64_t id, const JV& ok) { e2e::emitLine(object({{"id", id}, {"ok", ok}})); }
void fail(int64_t id, const std::string& why) { e2e::emitLine(object({{"id", id}, {"err", why}})); }
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    const auto url = argument(argc, argv, L"--url"), token = argument(argc, argv, L"--token"), session = argument(argc, argv, L"--session"), root = argument(argc, argv, L"--root");
    for (int i = 1; i < argc; ++i) { if (!wcscmp(argv[i], L"--no-scale")) scaleHeader = false; if (!wcscmp(argv[i], L"--no-onpaste")) onPasteHeader = false; if (!wcscmp(argv[i], L"--drop-waiting")) dropWaiting = true; if (i + 1 < argc && !wcscmp(argv[i], L"--emit-delay-ms")) emitDelayMs = _wtoi(argv[i + 1]); if (!wcscmp(argv[i], L"--no-pullqueue")) pullQueueHeader = false; if (!wcscmp(argv[i], L"--no-deferred")) deferredHeader = false; if (!wcscmp(argv[i], L"--quiet")) quiet = true; }
    const bool oleOk = SUCCEEDED(OleInitialize(nullptr));
    ClipboardWorkerAccess access;
    access.read = [](HWND w, bool t, bool f) { return board.read(w, clip::ReadOptions{t, f, false, false}); };
    access.readFormats = [](HWND w, const clip::ReadOptions& o) { return board.read(w, o); };
    access.putText = [](HWND w, const std::wstring& t, std::optional<DWORD> e) { return board.putText(w, t, e); };
    access.putFiles = [](HWND w, const std::vector<std::filesystem::path>& f, std::optional<DWORD> e) { return board.putFiles(w, f, e); };
    access.publishOffer = [](const std::shared_ptr<clip::PullOffer>& o, std::optional<DWORD> e) { return board.publishOffer(o, e); };
    access.offerCurrent = [](const std::shared_ptr<clip::PullOffer>& o) { return board.offerCurrent(o); };
    access.withdrawOffer = [](const std::shared_ptr<clip::PullOffer>& o) { return board.withdraw(o); };
    access.watch.add = [](HWND) { return true; };
    access.watch.remove = [](HWND) {};
    access.watch.sequence = [] { return board.sequence(); };
    access.watch.owner = [] { return board.owner(); };
    access.publishers.rich = [](HWND w, const std::wstring& t, const std::vector<uint8_t>* h, const std::vector<uint8_t>*, std::optional<DWORD> e) { return board.putRich(w, t, h, e); };
    access.publishers.image = [](HWND w, const clip::Image& i, std::optional<DWORD> e) { return board.putImage(w, i, e); };
    access.publishers.paths = [](HWND w, const std::vector<std::filesystem::path>& p, std::optional<DWORD> e) { return board.putFiles(w, p, e); };
    access.publishers.localTree = [](HWND, const std::vector<clip::TreeEntry>& entries, std::optional<DWORD> e) { return board.publishLocalTree(entries, e); };
    // The listener of the worker's window: every change of the stand-in clipboard is a WM_CLIPBOARDUPDATE there.
    // This process's worker window only: another helper of a run going on at the same time, or a real Host worker on this desktop, has
    // a window of the same class, and the first one found is not necessarily ours.
    board.changed = [] {
      for (HWND window = FindWindowExW(HWND_MESSAGE, nullptr, L"Multitap.Clipboard.Worker.v1", nullptr); window;
           window = FindWindowExW(HWND_MESSAGE, window, L"Multitap.Clipboard.Worker.v1", nullptr)) {
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId()) { PostMessageW(window, WM_CLIPBOARDUPDATE, 0, 0); return; }
      }
    };

    PlainChannel channel;
    std::unique_ptr<ClipboardWorker> worker;
    const auto packetPrefix = std::string("\"payload\":");
    worker = std::make_unique<ClipboardWorker>(session, [&](const std::string& packet) {
      const auto at = packet.find(packetPrefix);
      if (at == std::string::npos) {
        // hostAction 253: what the service would journal for administrators (clipboard_diagnostic.hpp).
        try {
          const auto value = e2e::parse(packet);
          if (value.str("type") == "clip.diag")
            e2e::emitLine(object({{"event", "diag"}, {"stage", value.num("stage")}, {"ok", value.flag("ok")}, {"error", value.num("error")}, {"cause", value.num("cause")},
                                  {"format", value.num("format")}, {"items", value.num("items")}, {"kib", value.num("kib")}}));
        } catch (...) {}
        return;
      }
      const auto payload = packet.substr(at + packetPrefix.size(), packet.size() - at - packetPrefix.size() - 1);
      // The service parses every worker packet before it forwards the payload (service.hpp, clip.packet).
      if (quiet) { (void)JsonParser::parse(packet); channel.send(payload); return; }
      try { const auto value = e2e::parse(payload); e2e::emitLine(object({{"event", "emit"}, {"type", value.str("type")}, {"requestId", value.str("requestId")}, {"reason", value.str("reason")}, {"active", value.flag("active")}}));
        if (dropWaiting && value.str("type") == "clipboard.paste.waiting") return;
        if (emitDelayMs > 0 && value.str("type") == "clipboard.chunk") Sleep(DWORD(emitDelayMs)); } catch (...) {}
      channel.send(payload);
    }, [] { return true; }, std::filesystem::path(e2e::wide(root)) / L"host-transfers", access);
    channel.flow_ = [&](uint64_t sent) { worker->message(JsonParser::parse("{\"type\":\"clipboard.flow\",\"sent\":" + std::to_string(sent) + "}")); };
    channel.start(url, token, [&](const std::string& json) {
      if (quiet) { worker->message(JsonParser::parse(json)); return; }
      try {
        const auto value = e2e::parse(json);
        e2e::emitLine(object({{"event", "recv"}, {"type", value.str("type")}, {"requestId", value.str("requestId")}, {"reason", value.str("reason")}, {"enabled", value.flag("enabled")}}));
      } catch (...) {}
      worker->message(JsonParser::parse(json));
    });
    e2e::emitLine(object({{"event", "started"}}));

    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      int64_t id = 0;
      try {
        const auto command = e2e::parse(line); id = command.num("id"); const auto name = command.str("cmd");
        if (e2e::handleRender(board, command, id)) continue;
        if (name == "ping") respond(id, true);
        else if (name == "cpu") { FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
          const auto ms = [](const FILETIME& t) { return int64_t(((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000); };
          respond(id, object({{"userMs", ms(user)}, {"kernelMs", ms(kernel)}})); }
        else if (name == "quit") { respond(id, true); break; }
        else if (name == "setText") { board.userText(e2e::wide(command.str("text"))); respond(id, true); }
        else if (name == "setRich") { const auto html = command.str("html"); board.userRich(e2e::wide(command.str("text")), std::vector<uint8_t>(html.begin(), html.end())); respond(id, true); }
        else if (name == "setImage") { clip::Image image; image.png = e2e::unbase64(command.str("png")); image.width = uint32_t(command.num("width")); image.height = uint32_t(command.num("height")); board.userImage(image); respond(id, true); }
        else if (name == "setScreenshot") { clip::Image image; image.png = e2e::unbase64(command.str("png")); image.width = uint32_t(command.num("width")); image.height = uint32_t(command.num("height")); std::vector<std::filesystem::path> files; if (const auto* list = command.find("paths")) if (const auto* a = std::get_if<e2e::JArray>(&list->v)) for (const auto& item : *a) files.emplace_back(e2e::wide(std::get<std::string>(item.v))); board.userScreenshot(image, files); respond(id, true); }
        else if (name == "setFiles") { std::vector<std::filesystem::path> files; if (const auto* list = command.find("paths")) if (const auto* a = std::get_if<e2e::JArray>(&list->v)) for (const auto& item : *a) files.emplace_back(e2e::wide(std::get<std::string>(item.v))); board.userFiles(files); respond(id, true); }
        else if (name == "setVirtual") { const auto* list = command.find("entries"); board.userVirtual(list ? e2e::virtualItems(*list) : std::vector<vsrc::Item>{}); respond(id, true); }
      else if (name == "clear") { board.userClear(); respond(id, true); }
      else if (name == "renumber") { board.renumber(); respond(id, true); }
        else if (name == "get") respond(id, board.describe());
        else if (name == "describePaths") { std::vector<std::filesystem::path> tops; if (const auto* list = command.find("paths")) if (const auto* a = std::get_if<e2e::JArray>(&list->v)) for (const auto& item : *a) tops.emplace_back(e2e::wide(std::get<std::string>(item.v))); respond(id, e2e::Board::describePaths(tops)); }
        else if (name == "paste") {
          IDataObject* source = board.pasteSource(); const auto destination = std::filesystem::path(e2e::wide(command.str("dest")));
          if (!source) { fail(id, "no offer on the clipboard"); continue; }
          const int slowMs = int(command.num("slowMs")); const bool discard = command.flag("discard");
          std::thread([id, source, destination, slowMs, discard] {
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            try { const auto bytes = e2e::pasteOffer(source, destination, slowMs, discard); respond(id, object({{"bytes", int64_t(bytes)}})); }
            catch (const std::exception& failure) { fail(id, failure.what()); }
            source->Release();
            if (com) CoUninitialize();
          }).detach();
        }
        else if (name == "makeSparse") { e2e::makeSparse(std::filesystem::path(e2e::wide(command.str("path"))), uint64_t(command.num("size")), command.str("tail")); respond(id, true); }
        else if (name == "pasteTail") {
          IDataObject* source = board.pasteSource(); if (!source) { fail(id, "no offer on the clipboard"); continue; }
          const auto entry = LONG(command.num("entry")); const auto length = uint64_t(command.num("length"));
          std::thread([id, source, entry, length] {
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            try { uint64_t size = 0; const auto bytes = e2e::pasteTail(source, entry, length, &size); respond(id, object({{"size", int64_t(size)}, {"tail", std::string(bytes.begin(), bytes.end())}})); }
            catch (const std::exception& failure) { fail(id, failure.what()); }
            source->Release();
            if (com) CoUninitialize();
          }).detach();
        } else fail(id, "unknown command");
      } catch (const std::exception& failure) { fail(id, failure.what()); }
    }
    channel.stop(); worker.reset();
    if (oleOk) OleUninitialize();
    return 0;
  } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
