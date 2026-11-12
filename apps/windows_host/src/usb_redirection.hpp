#pragma once
// usb-v1 transport on the Host (docs/usb-redirection.md): the loopback USB/IP
// server the VM's kernel driver connects to, the Host's side of the session's
// relay socket pair, and the per-session orchestration. The logic between them
// is usb_core.hpp; the driver is usb_driver.hpp.
//
// - The loopback listener exists only during a session (SO_EXCLUSIVEADDRUSE,
//   127.0.0.1, fresh port) and accepts only a connection whose owning process
//   is the System process (PID 4: the kernel's WSK socket of usbip2_ude). A
//   connection from any user process is closed unread, so a VM user cannot
//   present their own device to our server, and each device can be imported
//   exactly once.
// - Nothing is written to the driver's persistent attach list: attaching uses
//   the non-persistent plug-in request only.
// - Hostile or broken input never crashes the service: every read is bounded,
//   every length checked, and a bad connection only ends that device.
#include "usb_driver.hpp"
#include <iphlpapi.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")
#include "websocket_send.hpp"
#include "channel_redial.hpp"

namespace host::usb {
// The process owning the other end of an accepted loopback connection.
inline DWORD loopbackPeerProcess(SOCKET socket) {
  sockaddr_in peer{}, local{}; int peerSize = sizeof(peer), localSize = sizeof(local);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peerSize) || getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize)) return 0;
  if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 0;
  DWORD size = 0; std::vector<BYTE> data;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD result = GetExtendedTcpTable(data.empty() ? nullptr : data.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (result == NO_ERROR && !data.empty()) break;
    if ((result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR) || !size || size > 16 * 1024 * 1024) return 0;
    data.assign(size + 4096, 0); size = DWORD(data.size());
  }
  if (data.empty()) return 0;
  const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(data.data());
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto& row = table->table[i];
    if (row.dwLocalAddr == peer.sin_addr.s_addr && u_short(row.dwLocalPort) == peer.sin_port &&
        row.dwRemoteAddr == local.sin_addr.s_addr && u_short(row.dwRemotePort) == local.sin_port) return row.dwOwningPid;
  }
  return 0;
}
inline constexpr DWORD systemProcessId = 4;

// --- the loopback USB/IP server --------------------------------------------------------
class UsbIpServer {
 public:
  struct Access { std::function<bool(SOCKET)> trusted = [](SOCKET socket) { return loopbackPeerProcess(socket) == systemProcessId; }; };
  ~UsbIpServer() { stop(); }
  // Listens on 127.0.0.1 at a free port and serves [core]; returns the port, or 0.
  uint16_t start(UsbSessionCore& core, Access access = {}) {
    stop();
    SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET) return 0;
    BOOL exclusive = TRUE; sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
    int size = sizeof(address);
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) ||
        bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 4) ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) || !address.sin_port) { closesocket(listener); return 0; }
    core_ = &core; access_ = std::move(access); listener_ = listener; stopped_ = false;
    thread_ = std::thread([this] { run(); });
    return ntohs(address.sin_port);
  }
  void stop() {
    stopped_ = true;
    {
      std::lock_guard lock(mutex_);
      for (auto& connection : connections_) shutdown(connection->socket, SD_BOTH);
    }
    if (thread_.joinable()) thread_.join();
    std::vector<std::unique_ptr<Connection>> done;
    { std::lock_guard lock(mutex_); done.swap(connections_); }
    for (auto& connection : done) if (connection->thread.joinable()) connection->thread.join();
    if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
    core_ = nullptr;
  }
  uint32_t refused() const { return refused_; }
 private:
  struct Connection { SOCKET socket = INVALID_SOCKET; std::thread thread; std::mutex write; std::atomic<bool> finished{false}; };
  UsbSessionCore* core_ = nullptr; Access access_; SOCKET listener_ = INVALID_SOCKET; std::atomic<bool> stopped_{true}; std::thread thread_;
  std::mutex mutex_; std::vector<std::unique_ptr<Connection>> connections_; std::atomic<uint32_t> refused_{0};

  void run() {
    while (!stopped_) {
      fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable); timeval wait{0, 200000};
      const int ready = select(0, &readable, nullptr, nullptr, &wait);
      if (ready < 0) break;
      reap();
      if (!ready || stopped_) continue;
      const SOCKET client = accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) continue;
      if (!access_.trusted || !access_.trusted(client)) { ++refused_; recordUsbEvent(UsbStage::importRefused, false, 1); shutdown(client, SD_BOTH); closesocket(client); continue; }
      auto connection = std::make_unique<Connection>(); connection->socket = client;
      Connection* raw = connection.get();
      {
        std::lock_guard lock(mutex_);
        if (connections_.size() >= 16) { closesocket(client); continue; }
        connections_.push_back(std::move(connection));
      }
      raw->thread = std::thread([this, raw] { serve(*raw); raw->finished = true; });
    }
  }
  void reap() {
    std::vector<std::unique_ptr<Connection>> done;
    {
      std::lock_guard lock(mutex_);
      for (auto it = connections_.begin(); it != connections_.end();) { if ((*it)->finished) { done.push_back(std::move(*it)); it = connections_.erase(it); } else ++it; }
    }
    for (auto& connection : done) { if (connection->thread.joinable()) connection->thread.join(); closesocket(connection->socket); }
  }
  static bool receiveAll(SOCKET socket, uint8_t* out, size_t size, const std::atomic<bool>& stopped) {
    size_t at = 0;
    while (at < size) {
      if (stopped) return false;
      const int count = recv(socket, reinterpret_cast<char*>(out + at), int(std::min<size_t>(size - at, 1 << 20)), 0);
      if (count <= 0) return false;
      at += size_t(count);
    }
    return true;
  }
  bool send(Connection& connection, const Bytes& bytes) {
    std::lock_guard lock(connection.write);
    size_t at = 0;
    while (at < bytes.size()) {
      const int count = ::send(connection.socket, reinterpret_cast<const char*>(bytes.data() + at), int(std::min<size_t>(bytes.size() - at, 1 << 20)), 0);
      if (count <= 0) { shutdown(connection.socket, SD_BOTH); return false; }
      at += size_t(count);
    }
    return true;
  }
  void serve(Connection& connection) {
    DWORD timeout = 15000;
    setsockopt(connection.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(connection.socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    BOOL noDelay = TRUE; setsockopt(connection.socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    uint8_t common[usbipCommonSize];
    if (!receiveAll(connection.socket, common, sizeof(common), stopped_)) return;
    const auto op = decodeOpCommon(common, sizeof(common));
    if (!op || op->version != usbipVersion) return;
    if (op->code == opReqDevlist) {                       // an empty list: devices are only ever imported by busid
      uint8_t reserved[4]; if (!receiveAll(connection.socket, reserved, 4, stopped_)) return;
      Bytes reply = encodeOpCommon(opRepDevlist, stOk); put32(reply, 0); send(connection, reply); return;
    }
    if (op->code != opReqImport) return;
    uint8_t busIdBytes[usbipBusIdSize];
    if (!receiveAll(connection.socket, busIdBytes, sizeof(busIdBytes), stopped_)) return;
    const auto busId = decodeBusId(busIdBytes);
    std::optional<UsbSessionCore::Imported> imported;
    if (busId && core_) imported = core_->importDevice(*busId, [this, &connection](const Bytes& bytes) { return send(connection, bytes); }, [&connection] { shutdown(connection.socket, SD_BOTH); });
    if (!imported) { send(connection, encodeOpCommon(opRepImport, stNoDev)); return; }
    const uint8_t id = imported->id;
    if (!send(connection, encodeImportReply(imported->exported))) { core_->kernelClosed(id); return; }
    timeout = 0;                                           // requests wait as long as the VM is quiet
    setsockopt(connection.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::vector<uint8_t> header(usbipHeaderSize);
    while (!stopped_) {
      if (!receiveAll(connection.socket, header.data(), header.size(), stopped_)) break;
      const auto urb = decodeUrbHeader(header.data(), header.size());
      if (!urb) break;
      if (urb->command == cmdUnlink) { if (!core_->unlink(id, *urb)) break; continue; }
      Bytes data;
      if (urb->direction == 0 && urb->length > 0) {
        if (size_t(urb->length) > usbdefaults::totalLimit) break;
        data.resize(size_t(urb->length));
        if (!receiveAll(connection.socket, data.data(), data.size(), stopped_)) break;
      }
      if (!core_->submit(id, *urb, std::move(data))) break;
    }
    core_->kernelClosed(id);
  }
};

// --- the Host's side of the session's relay socket pair ----------------------------------
class UsbChannel {
 public:
  using Receive = std::function<void(bool text, Bytes)>;
  static constexpr size_t maximumQueue = 4 * 1024 * 1024, maximumMessage = frameHeaderSize + maxUrbBytes;
  ~UsbChannel() { stop(); }
  void stop() { std::lock_guard lifecycle(lifecycle_); close(); if (thread_.joinable()) thread_.join(); receive_ = {}; }
  // Queues one message; false (nothing queued) when the queue is full now.
  bool trySend(bool text, Bytes bytes) {
    {
      std::lock_guard lock(mutex_);
      if (stopped_ || bytes.size() > maximumMessage || bytes_ + bytes.size() > maximumQueue || queue_.size() >= 256) return false;
      bytes_ += bytes.size(); queue_.push_back({text, std::move(bytes)});
    }
    condition_.notify_one(); return true;
  }
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, Receive receive) {
    std::lock_guard lifecycle(lifecycle_); stop();
    if(!credentialOrigin(credentials).relayMatches(url,"/api/v1/hosts/sessions/"+sessionId+"/usb"))throw std::runtime_error("Invalid USB relay URL");
    receive_ = std::move(receive); stopped_ = false; status_ = 0;
    thread_ = std::thread([this, credentials, sessionId] {
      std::thread sender;
      try {
        const auto origin=verifiedCredentialOrigin(credentials); Internet session(httpSession()), connection(WinHttpConnect(session.handle, origin.hostname.c_str(), origin.port, 0));
        const auto path = L"/api/v1/hosts/sessions/" + wide(sessionId) + L"/usb";
        Internet request(WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, origin.flags()));
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)), "USB relay redirects");
        winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0), "USB relay WebSocket");
        const auto authorization = L"Authorization: Host " + wide(credentials.token) + L"\r\n";
        winCheck(WinHttpSendRequest(request.handle, authorization.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "USB relay connect");
        winCheck(WinHttpReceiveResponse(request.handle, nullptr), "USB relay response");
        DWORD status = 0, length = sizeof(status);
        winCheck(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX), "USB relay status");
        status_ = status;
        if (status != 101) throw std::runtime_error("USB relay unavailable");
        const auto web = WinHttpWebSocketCompleteUpgrade(request.handle, 0); if (!web) winCheck(FALSE, "USB relay upgrade");
        if (stopped_) { WinHttpCloseHandle(web); throw std::runtime_error("USB relay stopped"); }
        socket_ = web;
        sender = std::thread([this] {
          try {
            while (!stopped_) {
              Item item;
              { std::unique_lock lock(mutex_); condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); }); if (stopped_) break; item = std::move(queue_.front()); queue_.pop_front(); bytes_ -= item.bytes.size(); }
              const auto socket = socket_.load();
              if (!socket || sendWebSocketMessageOn(socket, item.text, item.bytes.data(), item.bytes.size()) != NO_ERROR) break;
            }
          } catch (...) { }
          close();
        });
        std::array<uint8_t, 65536> buffer{}; Bytes message; bool text = false;
        while (!stopped_) {
          DWORD count = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{}; const auto socket = socket_.load();
          if (!socket || WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &count, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
          const bool part = type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
          if (!part) throw std::runtime_error("Unexpected USB relay payload");
          const bool thisText = type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
          if (!message.empty() && thisText != text) throw std::runtime_error("Mixed USB relay message");
          text = thisText;
          if (message.size() + count > (text ? 65000 : maximumMessage)) throw std::runtime_error("USB relay message exceeds bounds");
          message.insert(message.end(), buffer.begin(), buffer.begin() + count);
          if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (receive_) receive_(text, std::move(message));
            message.clear();
          }
        }
      } catch (...) { }
      close(); if (sender.joinable()) sender.join();
      if (receive_) try { receive_(true, Bytes{'{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'u', 's', 'b', '.', 'c', 'h', 'a', 'n', 'n', 'e', 'l', '.', 'c', 'l', 'o', 's', 'e', 'd', '"', '}'}); } catch (...) { }
    });
  }
  // channel-resume-v1: the Broker answered the last dial with a refusal (not "not now", not a lost network).
  bool refused() const { return channelDialRefused(status_.load()); }
 private:
  struct Item { bool text = true; Bytes bytes; };
  std::atomic<DWORD> status_{0};
  std::atomic<bool> stopped_{true}; std::atomic<HINTERNET> socket_{nullptr}; std::thread thread_; Receive receive_;
  std::mutex mutex_; std::recursive_mutex lifecycle_; std::condition_variable condition_; std::deque<Item> queue_; size_t bytes_ = 0;
  void close() { stopped_ = true; if (const auto socket = socket_.exchange(nullptr)) WinHttpCloseHandle(socket); { std::lock_guard lock(mutex_); queue_.clear(); bytes_ = 0; } condition_.notify_all(); }
};
}  // namespace host::usb

namespace host {
// --- One stream session's USB redirection ----------------------------------------------------
// Observe the one existing PlugOut operation without changing logical cleanup,
// retrying an IOCTL or assuming whether kernel close also removed the port.
inline std::function<void(int)> observedUsbDetach(std::function<bool(int)> detach) {
  return [detach = std::move(detach)](int port) noexcept {
    if (!detach) return;
    uint32_t cause = 1;
    try { if (detach(port)) return; }
    catch (...) { cause = 2; }
    recordHostEvent("hostAction", {{"action", 250}, {"stage", int64_t(UsbStage::detachFailed)}, {"ok", 0}, {"cause", cause}});
  };
}

class UsbRedirection {
 public:
  struct Options {
    usb::DriverAccess driver = usb::nativeDriverAccess();
    std::function<int(const std::string&, uint16_t, const std::string&)> attach = [](const std::string& host, uint16_t port, const std::string& busId) { return usb::vhciAttach(host, port, busId); };
    std::function<bool(int)> detach = [](int port) { return usb::vhciDetach(port); };
    std::function<void()> cancelAttach = [] { usb::vhciCancelAttach(); };
    std::function<UsbVmState(uint16_t, uint16_t)> deviceState = [](uint16_t vendor, uint16_t product) { return usb::vmDeviceState(vendor, product); };
    usb::UsbIpServer::Access server;
  };
  ~UsbRedirection() { stop(); }
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, usb::Grant grant, Options options = {}) {
    std::lock_guard lifecycle(lifecycle_);
    stop();
    { std::lock_guard lock(mutex_); inbox_.clear(); inboxBytes_ = 0; closed_ = false; }
    credentials_ = credentials; url_ = url;
    options_ = std::move(options); sessionId_ = sessionId; grant_ = grant; stopped_ = false;
    channel_.start(credentials, sessionId, url, [this](bool text, usb::Bytes bytes) {
      {
        std::lock_guard lock(mutex_);
        if (text && std::string(bytes.begin(), bytes.end()) == "{\"type\":\"usb.channel.closed\"}") closed_ = true;
        else if (inbox_.size() >= 512 || inboxBytes_ + bytes.size() > 8 * 1024 * 1024) closed_ = true;
        else { inboxBytes_ += bytes.size(); inbox_.push_back({text, std::move(bytes)}); }
      }
      wake_.notify_all();
    });
    controller_ = std::thread([this] { run(); });
  }
  void stop() {
    std::lock_guard lifecycle(lifecycle_);
    stopped_ = true; preparing_ = false; wake_.notify_all();
    channel_.stop();
    if (controller_.joinable()) controller_.join();
  }
  bool active() const { return !stopped_; }
  // channel-resume-v1 (channel_redial.hpp): a fresh pair for the same session after the channel ended for a
  // transport reason. Every device of the ended pair was detached when it ended; the client attaches its
  // devices again on the new pair, through the same checks as the first time.
  void redial() {
    std::lock_guard lifecycle(lifecycle_);
    if (url_.empty()) throw std::runtime_error("usb_not_started");
    const auto credentials = credentials_; const auto sessionId = sessionId_; const auto url = url_; auto options = options_;
    start(credentials, sessionId, url, grant_, std::move(options));
  }
  // Told when the channel ended by itself (never by stop()). `transient`: the same session may pair again.
  // Called on the controller thread after it let everything go; it must not block.
  void onEnded(std::function<void(bool transient)> hook) { std::lock_guard lock(mutex_); ended_ = std::move(hook); }
 private:
  Credentials credentials_; std::string url_; std::function<void(bool)> ended_;
  struct Message { bool text = true; usb::Bytes bytes; };
  std::recursive_mutex lifecycle_;
  Options options_; std::string sessionId_; usb::Grant grant_;
  usb::UsbChannel channel_; usb::UsbIpServer server_; std::thread controller_;
  std::mutex mutex_; std::condition_variable wake_; std::deque<Message> inbox_; size_t inboxBytes_ = 0; bool closed_ = false;
  std::atomic<bool> stopped_{true}, preparing_{false};

  void run() {
    std::atomic<bool> stopping{false};
    uint16_t port = 0;
    UsbHooks hooks;
    hooks.sendFrame = [this](const usb::Bytes& bytes) { return channel_.trySend(false, bytes); };
    hooks.sendText = [this](const std::string& json) { return channel_.trySend(true, usb::Bytes(json.begin(), json.end())); };
    hooks.prepare = [this, &stopping]() -> UsbPrepared {
      const auto outcome = usb::prepareController(options_.driver, stopping);
      return {outcome == usb::Prepare::ready, usb::prepareReason(outcome)};
    };
    hooks.attach = [this, &port](const std::string& busId) { return options_.attach("127.0.0.1", port, busId); };
    hooks.detach = observedUsbDetach(options_.detach);
    hooks.cancelAttach = [this, &stopping] { stopping = true; options_.cancelAttach(); };
    hooks.deviceState = options_.deviceState;
    UsbSessionCore core(sessionId_, grant_, std::move(hooks));
    port = server_.start(core, options_.server);
    if (!port) recordUsbEvent(UsbStage::vhciUnavailable, false, uint32_t(WSAGetLastError()));
    // channel-resume-v1: why the pair ended. The Broker's own reason (usb.error), a violation seen here, or
    // neither (the transport just ended).
    std::string reason; bool errored = false, violated = false;
    for (;;) {
      std::deque<Message> messages; bool closed = false;
      {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopped_ || closed_ || !inbox_.empty(); });
        messages.swap(inbox_); inboxBytes_ = 0; closed = closed_ || stopped_;
      }
      for (auto& message : messages) {
        if (closed) break;
        try {
          if (message.text) {
            const auto parsed = JsonParser::parse(std::string(message.bytes.begin(), message.bytes.end()));
            if (parsed.get("type").string() == "usb.error") { errored = true; reason = parsed.get("reason").string(); }
            if (!core.message(parsed)) { closed = true; if (!errored) violated = true; }
          }
          else if (!core.frame(message.bytes.data(), message.bytes.size())) { closed = true; violated = true; }
        } catch (...) { closed = true; violated = true; }
      }
      core.tick();
      if (closed || !port) break;
    }
    const bool own = stopped_;
    stopping = true;
    core.closed();
    server_.stop();
    channel_.stop();
    // An end of its own (not stop()) is reported once everything is let go: a refused dial, a violation, a
    // reason of the pair's own or a listener that could not start is final.
    std::function<void(bool)> hook;
    if (!own) { std::lock_guard lock(mutex_); hook = ended_; }
    if (hook) hook(port != 0 && !violated && !channel_.refused() && (!errored || channelEndTransient("usb", reason)));
  }
};

// Removes controller ports left attached by a run that ended abnormally.
inline void removeUsbLeftovers() noexcept {
  try {
    if (usb::controllerState() != 1) return;
    if (const int count = usb::vhciDetachAll(); count > 0) recordUsbEvent(UsbStage::leftovers, true, 0, uint32_t(count));
  } catch (...) { }
}
class UsbLeftoverSweep {
 public:
  ~UsbLeftoverSweep() { if (thread_.joinable()) thread_.join(); }
  void start() { thread_ = std::thread([] { removeUsbLeftovers(); }); }
 private:
  std::thread thread_;
};
inline std::string usbCapabilityFields() { return usb::usbRedirectionAvailable() ? std::string(",\"usbProtocol\":\"") + usbProtocolName + '"' : std::string(); }
}  // namespace host
