// Nagle on the Host's WinHTTP WebSocket (tuning backlog #43). WinHTTP has no
// TCP_NODELAY option and does not expose its socket, and on loopback Windows
// shows no Nagle delay even for a socket that has it on (measured: a default
// Winsock client's second small message arrived 0.09 ms after the first), so a
// timing test cannot tell. WinHTTP's sockets live in this process, though:
// find the one connected to a local listener and read its TCP_NODELAY. Two raw
// Winsock sockets (default, and TCP_NODELAY set) prove the reading method.
// Exit 0: WinHTTP's WebSocket runs with TCP_NODELAY; 2: it runs with Nagle;
// 3: no verdict (socket not found or the controls disagree).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string acceptKey(const std::string& key) {
  const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  BCRYPT_ALG_HANDLE algorithm = nullptr; UCHAR digest[20]{};
  require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0, "SHA-1 provider");
  const auto status = BCryptHash(algorithm, nullptr, 0, PUCHAR(input.data()), ULONG(input.size()), digest, sizeof(digest));
  BCryptCloseAlgorithmProvider(algorithm, 0); require(status == 0, "SHA-1 hash");
  char text[64]{}; DWORD length = sizeof(text);
  require(CryptBinaryToStringA(digest, sizeof(digest), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text, &length), "Base64");
  return std::string(text, length);
}
// TCP_NODELAY of this process's socket whose peer is 127.0.0.1:port.
std::optional<bool> noDelayTowards(unsigned short port) {
  for (uintptr_t value = 4; value < 0x40000; value += 4) {
    const auto candidate = SOCKET(value);
    sockaddr_in peer{}; int size = sizeof(peer);
    if (getpeername(candidate, reinterpret_cast<sockaddr*>(&peer), &size) != 0 || peer.sin_family != AF_INET || ntohs(peer.sin_port) != port) continue;
    BOOL on = FALSE; int length = sizeof(on);
    if (getsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&on), &length) != 0) continue;
    return on != FALSE;
  }
  return std::nullopt;
}
struct Listener {
  SOCKET socket = INVALID_SOCKET; unsigned short port = 0;
  Listener() {
    socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); require(socket != INVALID_SOCKET, "Listener");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(socket, 1) == 0, "Listen");
    int length = sizeof(address); getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length); port = ntohs(address.sin_port);
  }
  ~Listener() { closesocket(socket); }
};
std::optional<bool> raw(bool noDelay) {
  Listener listener;
  SOCKET peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); require(peer != INVALID_SOCKET, "Client socket");
  if (noDelay) { BOOL on = TRUE; require(setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on)) == 0, "TCP_NODELAY"); }
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(listener.port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(connect(peer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Connect");
  const SOCKET accepted = accept(listener.socket, nullptr, nullptr);
  const auto result = noDelayTowards(listener.port);
  closesocket(accepted); closesocket(peer);
  return result;
}
std::optional<bool> winhttp() {
  Listener listener;
  std::thread server([&] {
    const SOCKET peer = accept(listener.socket, nullptr, nullptr); if (peer == INVALID_SOCKET) return;
    std::string request; char byte = 0;
    while (request.find("\r\n\r\n") == std::string::npos && recv(peer, &byte, 1, 0) == 1) request += byte;
    const auto at = request.find("Sec-WebSocket-Key: ");
    const auto key = request.substr(at + 19, request.find("\r\n", at) - at - 19);
    const auto response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n";
    send(peer, response.data(), int(response.size()), 0);
    while (recv(peer, &byte, 1, 0) == 1) {}
    closesocket(peer);
  });
  HINTERNET session = WinHttpOpen(L"MultitapHostNagleProbe", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", listener.port, 0);
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/api/v1/hosts/channel", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  std::optional<bool> result;
  if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (socket) {
      uint8_t message[24]{};
      (void)WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, message, sizeof(message));
      result = noDelayTowards(listener.port);
      WinHttpCloseHandle(socket);
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  if (session) WinHttpCloseHandle(session);
  shutdown(listener.socket, SD_BOTH); server.join();
  return result;
}
const char* text(const std::optional<bool>& value) { return !value ? "null" : *value ? "true" : "false"; }
}

int main() {
  try {
    WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock");
    const auto nagle = raw(false), noDelay = raw(true), host = winhttp();
    WSACleanup();
    std::cout << "{\"tcpNoDelay\":{\"winhttpWebSocket\":" << text(host) << ",\"rawDefault\":" << text(nagle) << ",\"rawSet\":" << text(noDelay) << "}}\n";
    if (nagle != std::optional<bool>(false) || noDelay != std::optional<bool>(true) || !host) return 3;
    return *host ? 0 : 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
