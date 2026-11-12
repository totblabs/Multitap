#pragma once
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace host {
// Every Host WebSocket (broker channel, track tunnel, clipboard)
// sends from one thread while another waits in WinHttpWebSocketReceive.
// WinHTTP answers the relay's PING from that receive and writes the PONG
// without waiting for a WinHttpWebSocketSend in progress on the sending
// thread. A frame larger than about 24-32 KiB is written in several socket
// writes, so the PONG can land inside its payload; the relay then parses
// payload bytes as frame headers ("RSV2 and RSV3 must be clear") and drops
// the whole connection as host_error. That ends every session of the Host
// while a 4K video streams at ~30 Mbps. Reproduced with WinHTTP
// and the relay's ws 8.22 settings, plain and TLS, idle and under full CPU
// load: whole 200 KB-1.5 MB frames broke on the first ping, frames of at most
// 24 KiB never did (thousands of pings). So every message leaves as frames of
// at most webSocketFragmentBytes; a control frame may legally sit between
// them. Each message is one call here, on its connection's only sending
// thread, so fragments of two messages never interleave.
inline constexpr size_t webSocketFragmentBytes = 8192;

// Sends `size` bytes as one WebSocket message through `send(type, data, n)`
// (WinHttpWebSocketSend's signature without the handle; it returns a Win32
// error, NO_ERROR on success). A text message is cut only before a UTF-8 lead
// byte, so each fragment holds whole characters. Returns the first error.
template<class Send>
DWORD sendWebSocketMessage(Send&& send, bool text, const uint8_t* data, size_t size, size_t fragment = webSocketFragmentBytes) {
  const auto part = text ? WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE : WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
  const auto last = text ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
  if (size > MAXDWORD) return ERROR_INVALID_PARAMETER;
  fragment = std::max<size_t>(fragment, 4);  // room for one whole UTF-8 sequence
  size_t at = 0;
  while (size - at > fragment) {
    size_t end = at + fragment;
    if (text) while (end > at + 1 && (data[end] & 0xC0) == 0x80) --end;
    if (const DWORD error = send(part, data + at, DWORD(end - at)); error != NO_ERROR) return error;
    at = end;
  }
  return send(last, data + at, DWORD(size - at));
}
// The same on a WinHTTP WebSocket handle.
inline DWORD sendWebSocketMessageOn(HINTERNET socket, bool text, const void* data, size_t size) {
  if (!socket) return ERROR_INVALID_HANDLE;
  return sendWebSocketMessage([socket](WINHTTP_WEB_SOCKET_BUFFER_TYPE type, const uint8_t* bytes, DWORD count) {
    return WinHttpWebSocketSend(socket, type, const_cast<uint8_t*>(bytes), count);
  }, text, static_cast<const uint8_t*>(data), size);
}
}
