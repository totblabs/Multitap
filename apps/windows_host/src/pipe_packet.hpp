#pragma once
#include "machine.hpp"
#include "stream_resize.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

namespace host {
// The worker <-> service pipe is a byte stream of packets: u32 length (kind byte
// + payload), u8 kind (0 text, 1 media), payload. The worker's capture/encode
// thread writes every encoded frame here synchronously (Agent local_ sink), and
// the service reads it at normal priority. With the former 64 KiB pipe buffer a
// 700 KB frame needed a dozen service wake-ups inside the encode stage and any
// scheduling delay of the service's reader (busy guest, vCPU contention) was
// added to the capture thread's frame time (measured by pipe_write_benchmark).
// The buffer now holds one maximum packet (4 MiB + header), so a frame is queued
// at once and the reader drains it afterwards. Only the worker -> service
// direction (the service's inbound side) is large; the service -> worker
// direction carries small control messages.
inline constexpr DWORD workerPipeInboundBytes = 4u * 1024 * 1024 + 64 * 1024;
inline constexpr DWORD workerPipeOutboundBytes = 65536;
// Work a reader serves while it waits: on a still screen no packet ends the wait.
// false from run ends the read.
struct PipeWake { HANDLE event = nullptr; std::function<bool()> run; };
inline bool pipeBytes(HANDLE pipe, bool writing, void* buffer, DWORD size, const std::atomic<bool>& stopped, std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(), const std::atomic<int64_t>* resizeDeadline = nullptr, const PipeWake* wake = nullptr) {
  auto* at = static_cast<BYTE*>(buffer);
  if (!size || stopped) return size == 0;
  // One event for every partial transfer of this call (it used to be created and
  // closed per iteration: a syscall pair for every 64 KiB hop).
  WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.value) return false;
  while (size && !stopped) {
    OVERLAPPED operation{}; operation.hEvent = event.value; DWORD transferred = 0;
    BOOL result = writing ? WriteFile(pipe, at, size, &transferred, &operation) : ReadFile(pipe, at, size, &transferred, &operation);
    if (!result && GetLastError() == ERROR_IO_PENDING) {
      const auto begun = Clock::now();
      const HANDLE waits[] = {event.value, wake ? wake->event : nullptr};
      for (;;) {
        const DWORD signaled = WaitForMultipleObjects(wake ? 2 : 1, waits, FALSE, 100);
        if (signaled == WAIT_OBJECT_0 + 1) {
          // The pending operation uses this frame: nothing may unwind past it.
          bool serving = false; try { serving = wake->run(); } catch (...) {}
          if (!serving) { CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &transferred, TRUE); return false; }
          continue;
        }
        if (signaled != WAIT_TIMEOUT) break;
        const auto deadline=resizeDeadline?resizeDeadline->load():0;
        const bool resizeExpired=deadline>0&&deadline<=streamMonotonicMillis();
        const bool idleExpired=timeout.count()>0&&Clock::now()-begun>=timeout&&deadline==0;
        if (stopped || (writing && Clock::now() - begun > std::chrono::seconds(5)) || resizeExpired || idleExpired) { CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &transferred, TRUE); return false; }
      }
      result = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    }
    if (!result || !transferred) return false; at += transferred; size -= transferred;
  }
  return size == 0;
}
inline bool pipePacket(HANDLE pipe, bool text, std::vector<uint8_t> bytes, const std::atomic<bool>& stopped, std::mutex& mutex,const std::atomic<int64_t>* resizeDeadline=nullptr) {
  if (bytes.empty() || bytes.size() > 4 * 1024 * 1024) return false;
  std::lock_guard lock(mutex);
  // Length and kind leave in one 5-byte write (the byte stream is unchanged).
  const uint32_t length = uint32_t(bytes.size() + 1); BYTE header[5] = {BYTE(length), BYTE(length >> 8), BYTE(length >> 16), BYTE(length >> 24), BYTE(text ? 0 : 1)};
  return pipeBytes(pipe, true, header, 5, stopped,{},resizeDeadline) && pipeBytes(pipe, true, bytes.data(), DWORD(bytes.size()), stopped,{},resizeDeadline);
}
inline bool readPipePacket(HANDLE pipe, bool& text, std::vector<uint8_t>& bytes, const std::atomic<bool>& stopped, std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(), const std::atomic<int64_t>* resizeDeadline = nullptr, const PipeWake* wake = nullptr) {
  uint32_t length = 0; BYTE kind = 0;
  if (!pipeBytes(pipe, false, &length, 4, stopped,timeout,resizeDeadline,wake) || length < 2 || length > 4 * 1024 * 1024 + 1 || !pipeBytes(pipe, false, &kind, 1, stopped,timeout,resizeDeadline,wake) || kind > 1) return false;
  bytes.resize(length - 1); text = kind == 0; return pipeBytes(pipe, false, bytes.data(), DWORD(bytes.size()), stopped,timeout,resizeDeadline,wake);
}
}