#pragma once
// Frame transport of the Role::secure helper: one unnamed shared section.
//
//   [ FrameHeader (64 bytes) ][ BGRA frame (width*height*4, top-down) ]
//
// The SERVICE creates the section and one auto-reset event without names, lets
// the SYSTEM helper inherit them (read/write) and duplicates a READ-ONLY section
// handle and a wait-only event handle into the user worker it launched. Nothing
// is reachable by name, so no other process of the user can read the prompt's
// pixels or replace them. Input never travels here: the service writes it to the
// helper's private pipe (secure_desktop_launch.hpp).
//
// The helper is the only writer. A frame is published under a sequence lock
// (seq odd while the pixels are written), so the worker never encodes a torn
// frame. `alive` is the helper's heartbeat (a still prompt produces no frames);
// `state` is its last word before it exits by itself.
//
// Pure memory layout and accessors; no Windows calls, so the tests drive both
// ends over an ordinary buffer.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace host::secure::ipc {

inline constexpr uint32_t frameMagic = 0x53454333u;  // 'SEC3'
inline constexpr uint32_t stateRunning = 0;  // capturing (or not started yet)
inline constexpr uint32_t stateEnded = 1;    // exited by itself: the input desktop is Default again
inline constexpr uint32_t stateFailed = 2;   // exited by itself: scope lost, duplication failed, transport closed

#pragma pack(push, 4)
struct FrameHeader {
  uint32_t magic;
  volatile uint32_t seq;    // even: a complete frame (0: none yet); odd: being written
  uint32_t width;
  uint32_t height;
  volatile uint32_t alive;  // heartbeat, incremented every helper loop tick
  volatile uint32_t state;  // stateRunning / stateEnded / stateFailed
  // Input the helper took from the service's pipe, as counts only (never a key or a position): what arrived,
  // what the allowlist dropped, what Windows accepted, what was refused before Windows was asked (a malformed
  // event or a position outside the owned output), how often SendInput itself failed and its last error.
  volatile uint32_t inputReceived, inputDropped, inputInjected, inputRefused, inputSendFailed, inputLastError;
  volatile uint32_t inputOffDesktop;  // system injector only: not injected because the input desktop was not the user's
  uint32_t reserved[3];
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 64, "FrameHeader is 64 bytes");

inline constexpr uint32_t minSide = 16, maxSide = 8192;
inline bool sizeValid(uint32_t width, uint32_t height) {
  return width >= minSide && width <= maxSide && height >= minSide && height <= maxSide;
}
inline size_t frameBytes(uint32_t width, uint32_t height) { return size_t(width) * height * 4; }
inline size_t sectionBytes(uint32_t width, uint32_t height) { return sizeof(FrameHeader) + frameBytes(width, height); }

// ---- writer (the helper) ----------------------------------------------------
inline void writerInit(uint8_t* view, uint32_t width, uint32_t height) {
  auto* header = reinterpret_cast<FrameHeader*>(view);
  header->width = width;
  header->height = height;
  header->state = stateRunning;
  header->magic = frameMagic;
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
inline void writeFrame(uint8_t* view, const uint8_t* bgra, uint32_t width, uint32_t height) {
  auto* header = reinterpret_cast<FrameHeader*>(view);
  if (header->width != width || header->height != height) return;
  const uint32_t seq = header->seq;
  header->seq = seq | 1u;  // odd: writing
  std::atomic_thread_fence(std::memory_order_seq_cst);
  std::memcpy(view + sizeof(FrameHeader), bgra, frameBytes(width, height));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint32_t next = (seq | 1u) + 1u;  // even
  if (!next) next = 2;              // 0 means "no frame yet"
  header->seq = next;
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
inline void writeAlive(uint8_t* view) {
  auto* header = reinterpret_cast<FrameHeader*>(view);
  header->alive = header->alive + 1u;
}
struct InputCounts {
  uint32_t received = 0, dropped = 0, injected = 0, refused = 0, sendFailed = 0, lastError = 0, offDesktop = 0;
  bool operator==(const InputCounts&) const = default;
};
inline void writeInputCounts(uint8_t* view, const InputCounts& counts) {
  auto* header = reinterpret_cast<FrameHeader*>(view);
  header->inputReceived = counts.received; header->inputDropped = counts.dropped; header->inputInjected = counts.injected;
  header->inputRefused = counts.refused; header->inputSendFailed = counts.sendFailed; header->inputLastError = counts.lastError;
  header->inputOffDesktop = counts.offDesktop;
}
inline void writeState(uint8_t* view, uint32_t state) {
  reinterpret_cast<FrameHeader*>(view)->state = state;
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

// ---- reader (the worker) ----------------------------------------------------
struct Reader {
  const uint8_t* view = nullptr;
  uint32_t width = 0, height = 0;
  uint32_t lastSeq = 0;

  const FrameHeader* header() const { return reinterpret_cast<const FrameHeader*>(view); }
  bool ready() const { return view && header()->magic == frameMagic && header()->width == width && header()->height == height; }
  uint32_t alive() const { return view ? header()->alive : 0u; }
  uint32_t state() const { return view ? header()->state : stateRunning; }
  InputCounts inputCounts() const {
    if (!view) return {};
    const auto* h = header();
    return {h->inputReceived, h->inputDropped, h->inputInjected, h->inputRefused, h->inputSendFailed, h->inputLastError, h->inputOffDesktop};
  }

  // True and fills `bgra` when a complete frame newer than the last one read is
  // there. A frame the helper is writing (or rewrote during the copy) is left
  // for the next call; `bgra` then keeps its previous content.
  bool read(std::vector<uint8_t>& bgra, std::vector<uint8_t>& scratch) {
    if (!ready()) return false;
    const size_t bytes = frameBytes(width, height);
    for (int attempt = 0; attempt < 3; ++attempt) {
      const uint32_t before = header()->seq;
      if (!before || (before & 1u) || before == lastSeq) return false;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      scratch.resize(bytes);
      std::memcpy(scratch.data(), view + sizeof(FrameHeader), bytes);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (header()->seq != before) continue;  // rewritten during the copy
      bgra.swap(scratch);
      lastSeq = before;
      return true;
    }
    return false;
  }
};

}  // namespace host::secure::ipc
