#pragma once
// drive-v1 relay socket of the Session-0 service (the clipboard channel's
// pattern, ClipboardChannel): its own WSS to /api/v1/hosts/sessions/<id>/drive
// with this VM's Host token, text and binary messages, a bounded send queue.
// Callbacks run on the channel's receive thread; send never calls back.
#include "machine.hpp"
#include "channel_redial.hpp"
#include <condition_variable>
#include <deque>
#include <functional>

namespace host::drive {
class Channel {
 public:
  using Text = std::function<void(std::string)>;
  using Binary = std::function<void(std::vector<uint8_t>)>;
  using Closed = std::function<void()>;
  static constexpr size_t maxText = 65536, maxBinary = 288 * 1024, maxQueued = 64, maxQueuedBytes = 4 * 1024 * 1024;
  ~Channel() { stop(); }
  // The URL must be exactly this session's drive path on the broker origin.
  static bool validUrl(const Credentials& credentials, const std::string& sessionId, const std::string& url) {
    try {
      return credentialOrigin(credentials).relayMatches(url,"/api/v1/hosts/sessions/"+sessionId+"/drive");
    } catch (...) { return false; }
  }
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, Text text, Binary binary, Closed closed) {
    std::lock_guard lifecycle(lifecycle_); stop();
    if (!validUrl(credentials, sessionId, url)) throw std::runtime_error("Invalid drive relay URL");
    text_ = std::move(text); binary_ = std::move(binary); closed_ = std::move(closed); stopped_ = false; status_ = 0;
    thread_ = std::thread([this, credentials, sessionId] {
      std::thread sender;
      try {
        const auto origin=verifiedCredentialOrigin(credentials); Internet session(httpSession()), connection(WinHttpConnect(session.handle, origin.hostname.c_str(), origin.port, 0));
        const auto path = L"/api/v1/hosts/sessions/" + wide(sessionId) + L"/drive";
        Internet request(WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, origin.flags()));
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)), "Drive relay redirects");
        winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0), "Drive relay WebSocket");
        const auto authorization = L"Authorization: Host " + wide(credentials.token) + L"\r\nX-Multitap-Drive-Open: drive.open-v1\r\n";
        winCheck(WinHttpSendRequest(request.handle, authorization.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "Drive relay connect");
        winCheck(WinHttpReceiveResponse(request.handle, nullptr), "Drive relay response");
        DWORD status = 0, length = sizeof(status);
        winCheck(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX), "Drive relay status");
        status_ = status;
        if (status != 101) throw std::runtime_error("Drive relay unavailable");
        const auto web = WinHttpWebSocketCompleteUpgrade(request.handle, 0); if (!web) winCheck(FALSE, "Drive relay upgrade");
        if (stopped_) { WinHttpCloseHandle(web); throw std::runtime_error("Drive relay stopped"); }
        socket_ = web;
        sender = std::thread([this] {
          try {
            while (!stopped_) {
              Message message;
              { std::unique_lock lock(mutex_); condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); }); if (stopped_) break; message = std::move(queue_.front()); queue_.pop_front(); bytes_ -= message.bytes.size(); }
              const auto socket = socket_.load();
              if (!socket || WinHttpWebSocketSend(socket, message.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, message.bytes.data(), DWORD(message.bytes.size())) != NO_ERROR) break;
            }
          } catch (...) { }
          close();
        });
        std::vector<uint8_t> buffer(65536), message; bool binary = false;
        while (!stopped_) {
          DWORD count = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
          const auto socket = socket_.load();
          if (!socket || WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &count, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
          const bool isBinary = type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
          if (message.empty()) binary = isBinary; else if (binary != isBinary) throw std::runtime_error("Mixed drive relay message");
          if (message.size() + count > (binary ? maxBinary : maxText)) throw std::runtime_error("Drive relay message exceeds bounds");
          message.insert(message.end(), buffer.begin(), buffer.begin() + count);
          if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) { if (text_) text_(std::string(message.begin(), message.end())); message.clear(); }
          else if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) { if (binary_) binary_(std::move(message)); message.clear(); }
        }
      } catch (...) { }
      close(); if (sender.joinable()) sender.join();
      if (closed_) try { closed_(); } catch (...) { }
    });
  }
  bool send(std::string text) { return enqueue(false, std::vector<uint8_t>(text.begin(), text.end())); }
  bool sendBinary(std::vector<uint8_t> data) { return enqueue(true, std::move(data)); }
  void stop() { std::lock_guard lifecycle(lifecycle_); close(); if (thread_.joinable()) thread_.join(); text_ = {}; binary_ = {}; closed_ = {}; }
  // Ends the socket without waiting; safe from this channel's own callbacks.
  void abort() { close(); }
  bool open() const { return !stopped_ && socket_.load() != nullptr; }
  // channel-resume-v1: the Broker answered the last dial with a refusal (not "not now", not a lost network).
  bool refused() const { return channelDialRefused(status_.load()); }
 private:
  std::atomic<DWORD> status_{0};
  struct Message { bool binary = false; std::vector<uint8_t> bytes; };
  std::atomic<bool> stopped_{true}; std::atomic<HINTERNET> socket_{nullptr}; std::thread thread_; Text text_; Binary binary_; Closed closed_;
  std::mutex mutex_; std::recursive_mutex lifecycle_; std::condition_variable condition_; std::deque<Message> queue_; size_t bytes_ = 0;
  bool enqueue(bool binary, std::vector<uint8_t> bytes) {
    bool overflow = false;
    { std::lock_guard lock(mutex_); if (stopped_) return false;
      if (bytes.size() > (binary ? maxBinary : maxText) || queue_.size() >= maxQueued || bytes_ + bytes.size() > maxQueuedBytes) overflow = true;
      else { bytes_ += bytes.size(); queue_.push_back({binary, std::move(bytes)}); } }
    if (overflow) { close(); return false; }
    condition_.notify_one(); return true;
  }
  void close() { stopped_ = true; if (const auto socket = socket_.exchange(nullptr)) WinHttpCloseHandle(socket); { std::lock_guard lock(mutex_); queue_.clear(); bytes_ = 0; } condition_.notify_all(); }
};
}
