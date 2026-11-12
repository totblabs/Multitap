// Host WebSocket fragmentation (websocket_send.hpp).
// 1. Splitting rules with a recording sender (no network).
// 2. The real failure on loopback: actual WinHTTP sends 1.5 MB messages on one
//    thread while another waits in WinHttpWebSocketReceive, and a local
//    server pings every millisecond and checks every frame it receives as the
//    relay's ws library does (RSV, opcode, mask, control frame rules). With
//    the production fragments no frame may be malformed and every message
//    must arrive intact. Whole frames are run once for comparison and only
//    reported (they break at the first ping).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "websocket_send.hpp"
#include <bcrypt.h>
#include <wincrypt.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Call { WINHTTP_WEB_SOCKET_BUFFER_TYPE type; std::string bytes; };
std::vector<Call> split(bool text, const std::string& message, size_t fragment = host::webSocketFragmentBytes, int failAt = -1) {
  std::vector<Call> calls;
  const DWORD result = host::sendWebSocketMessage([&](WINHTTP_WEB_SOCKET_BUFFER_TYPE type, const uint8_t* data, DWORD size) -> DWORD {
    if (int(calls.size()) == failAt) return ERROR_WINHTTP_CONNECTION_ERROR;
    calls.push_back({type, std::string(reinterpret_cast<const char*>(data), size)}); return NO_ERROR;
  }, text, reinterpret_cast<const uint8_t*>(message.data()), message.size(), fragment);
  require((failAt < 0) == (result == NO_ERROR), "Unexpected send result");
  return calls;
}
bool validUtf8(const std::string& value) {
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0) > 0 || value.empty();
}
void splittingRules() {
  constexpr auto binaryPart = WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE, binaryLast = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
  constexpr auto textPart = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE, textLast = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
  require(host::webSocketFragmentBytes <= 16384, "Fragment bound above the measured safe size");
  { auto calls = split(false, std::string(20000, 'x'));
    require(calls.size() == 3 && calls[0].type == binaryPart && calls[1].type == binaryPart && calls[2].type == binaryLast &&
            calls[0].bytes.size() == 8192 && calls[1].bytes.size() == 8192 && calls[2].bytes.size() == 3616, "Binary message was not split into 8 KiB fragments"); }
  { auto calls = split(false, std::string(8192, 'x')); require(calls.size() == 1 && calls[0].type == binaryLast && calls[0].bytes.size() == 8192, "A message of exactly one fragment was split"); }
  { auto calls = split(false, std::string(8193, 'x')); require(calls.size() == 2 && calls[0].bytes.size() == 8192 && calls[1].type == binaryLast && calls[1].bytes.size() == 1, "One byte over the bound was not a final fragment"); }
  { auto calls = split(true, ""); require(calls.size() == 1 && calls[0].type == textLast && calls[0].bytes.empty(), "An empty message was not one final frame"); }
  { // A three-byte character across the 8 KiB boundary stays whole.
    const std::string message = std::string(8190, 'a') + "\xEA\xB0\x80" + "b";
    auto calls = split(true, message);
    require(calls.size() == 2 && calls[0].type == textPart && calls[0].bytes.size() == 8190 && calls[1].type == textLast, "Text was cut inside a UTF-8 character");
    for (const auto& call : calls) require(validUtf8(call.bytes), "A text fragment is not valid UTF-8"); }
  { // Random sizes and multibyte text reassemble exactly, every fragment bounded.
    uint32_t seed = 12345; const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    for (int round = 0; round < 200; ++round) {
      const bool text = round % 2; std::string message;
      const size_t length = next() % 70000;
      while (message.size() < length) message += text ? (next() % 3 ? std::string("\xED\x95\x9C") : std::string("z")) : std::string(1, char(next()));
      const auto calls = split(text, message, host::webSocketFragmentBytes);
      std::string joined; for (size_t n = 0; n < calls.size(); ++n) {
        require(calls[n].bytes.size() <= host::webSocketFragmentBytes, "A fragment exceeded the bound");
        require((n + 1 == calls.size()) == (calls[n].type == (text ? textLast : binaryLast)), "Only the last fragment may end the message");
        if (text) require(validUtf8(calls[n].bytes), "A random text fragment split a character");
        joined += calls[n].bytes; }
      require(joined == message, "Fragments did not reassemble the message");
    } }
  { auto calls = split(false, std::string(30000, 'x'), host::webSocketFragmentBytes, 1); require(calls.size() == 1, "A failed fragment did not stop the message"); }
}

// ------------------------------------------------------------ loopback check
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
struct Result { size_t messages = 0, frames = 0, pings = 0, pongs = 0; bool malformed = false, corrupt = false; std::string reason; };
bool readExact(SOCKET socket, uint8_t* data, size_t size) {
  while (size) { const int got = recv(socket, reinterpret_cast<char*>(data), int(std::min<size_t>(size, 1 << 20)), 0); if (got <= 0) return false; data += got; size -= size_t(got); }
  return true;
}
// Parses the client's frames as the relay's ws Receiver validates them.
void serve(SOCKET client, size_t expectedMessages, size_t messageBytes, Result& result, std::atomic<bool>& done) {
  std::string request; char byte = 0;
  while (request.find("\r\n\r\n") == std::string::npos && recv(client, &byte, 1, 0) == 1) request += byte;
  const auto at = request.find("Sec-WebSocket-Key: "); require(at != std::string::npos, "Missing WebSocket key");
  const auto key = request.substr(at + 19, request.find("\r\n", at) - at - 19);
  const auto response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n";
  send(client, response.data(), int(response.size()), 0);
  std::thread pinger([&] { static constexpr unsigned char ping[2] = {0x89, 0}; while (!done) { if (send(client, reinterpret_cast<const char*>(ping), 2, 0) != 2) break; ++result.pings; std::this_thread::sleep_for(std::chrono::milliseconds(1)); } });
  std::vector<uint8_t> payload; size_t messageSize = 0; bool inMessage = false;
  while (result.messages < expectedMessages) {
    uint8_t head[2]; if (!readExact(client, head, 2)) { result.reason = "connection ended"; break; }
    const bool fin = head[0] & 0x80; const int rsv = (head[0] >> 4) & 7, opcode = head[0] & 15; const bool masked = head[1] & 0x80; uint64_t length = head[1] & 127;
    if (length == 126) { uint8_t ext[2]; if (!readExact(client, ext, 2)) break; length = (uint64_t(ext[0]) << 8) | ext[1]; }
    else if (length == 127) { uint8_t ext[8]; if (!readExact(client, ext, 8)) break; length = 0; for (auto value : ext) length = (length << 8) | value; }
    const bool control = opcode >= 8;
    if (rsv || !masked || (opcode > 2 && opcode < 8) || opcode > 10 || (control && (!fin || length > 125)) || (opcode == 0 && !inMessage) || ((opcode == 1 || opcode == 2) && inMessage) || length > 8 * 1024 * 1024) {
      result.malformed = true; result.reason = "invalid frame header"; break;
    }
    uint8_t mask[4]; if (!readExact(client, mask, 4)) break;
    payload.resize(size_t(length)); if (length && !readExact(client, payload.data(), payload.size())) break;
    for (size_t n = 0; n < payload.size(); ++n) payload[n] ^= mask[n & 3];
    ++result.frames;
    if (opcode == 10) { ++result.pongs; continue; }
    if (opcode == 8) { result.reason = "client closed"; break; }
    if (opcode == 9) continue;
    // Data: the sender's pattern is byte n of a message = n * 131 + 7.
    for (size_t n = 0; n < payload.size(); ++n) if (payload[n] != uint8_t((messageSize + n) * 131 + 7)) { result.corrupt = true; break; }
    if (result.corrupt) { result.reason = "payload bytes changed"; break; }
    messageSize += payload.size(); inMessage = !fin;
    if (fin) { if (messageSize != messageBytes) { result.corrupt = true; result.reason = "message size changed"; break; } ++result.messages; messageSize = 0; }
  }
  done = true; pinger.join(); shutdown(client, SD_BOTH);
}
Result loopback(size_t fragment, size_t messages, size_t messageBytes) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); require(listener != INVALID_SOCKET, "Listener socket");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "Listen");
  int size = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size); const auto port = ntohs(address.sin_port);
  Result result; std::atomic<bool> done{false};
  std::thread server([&] { const SOCKET client = accept(listener, nullptr, nullptr); if (client == INVALID_SOCKET) return; try { serve(client, messages, messageBytes, result, done); } catch (const std::exception& error) { result.reason = error.what(); done = true; } closesocket(client); });
  HINTERNET session = WinHttpOpen(L"MultitapHostWebSocketTest", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/api/v1/hosts/channel", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  require(request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr), "WinHTTP upgrade request");
  HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0); require(socket != nullptr, "WinHTTP upgrade");
  // Like Agent::run: a receive is pending while the sender writes; WinHTTP
  // answers the server's pings from here.
  std::thread receiver([&] { BYTE buffer[4096]; while (!done) { DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{}; if (WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &got, &type) != NO_ERROR) break; } });
  std::vector<uint8_t> message(messageBytes); for (size_t n = 0; n < message.size(); ++n) message[n] = uint8_t(n * 131 + 7);
  for (size_t sent = 0; sent < messages && !done; ++sent) {
    const DWORD error = host::sendWebSocketMessage([&](WINHTTP_WEB_SOCKET_BUFFER_TYPE type, const uint8_t* data, DWORD count) { return WinHttpWebSocketSend(socket, type, const_cast<uint8_t*>(data), count); }, false, message.data(), message.size(), fragment);
    if (error != NO_ERROR) break;
  }
  for (int wait = 0; wait < 500 && !done; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  done = true; WinHttpCloseHandle(socket); receiver.join(); server.join();
  WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); closesocket(listener);
  return result;
}
}
int main() {
  try {
    splittingRules();
    WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock");
    constexpr size_t messages = 300, messageBytes = 1500000;
    const auto fragmented = loopback(host::webSocketFragmentBytes, messages, messageBytes);
    require(!fragmented.malformed && !fragmented.corrupt, ("Fragmented sends produced a frame the relay rejects: " + fragmented.reason).c_str());
    require(fragmented.messages == messages, ("Fragmented messages did not all arrive: " + fragmented.reason).c_str());
    require(fragmented.pongs > 0, "No ping was answered while sending (the race was not exercised)");
    const auto whole = loopback(size_t(-1), 40, messageBytes);
    std::cout << "{\"splittingCases\":\"ok\",\"fragmentBytes\":" << host::webSocketFragmentBytes << ",\"messages\":" << fragmented.messages << ",\"frames\":" << fragmented.frames
              << ",\"pingsDuringSend\":" << fragmented.pings << ",\"pongs\":" << fragmented.pongs << ",\"malformedFrames\":0"
              << ",\"wholeFrameBaseline\":{\"messages\":" << whole.messages << ",\"broken\":" << ((whole.malformed || whole.corrupt) ? "true" : "false") << "},\"network\":\"loopback, no TLS\"}\n";
    WSACleanup();
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
