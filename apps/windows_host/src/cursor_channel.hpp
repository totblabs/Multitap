#pragma once
// ToTBSeamless cursor separation (cursor-v1; docs/remote-session-protocol.md
// "Candidate cursor separation"). In a granted session the capture leaves the
// cursor out of the video and this channel describes it instead: the shape in
// the native Windows cursor format (so the client recreates the identical
// cursor, including AND-mask transparency and XOR inversion), visibility, and a
// position only when the remote cursor moved without our injected input.
// Header-only: agent.hpp is included by many host targets.
#include <windows.h>
#include "owned_output_size.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace host::cursor {
inline constexpr const char* protocol = "cursor-v1";
// Independent outputs (tracks-v1): granted only together with cursor-v1.
inline constexpr const char* tracksProtocol = "cursor-tracks-v1";
inline constexpr uint8_t mediaKind = 3;
inline constexpr uint8_t shapeRecordType = 1, stateRecordType = 2;
inline constexpr uint16_t maximumSide = 256;
inline constexpr uint32_t maximumShapeId = 2147483647u;
// Shapes the host may still name in a state; the client caches more (64).
inline constexpr size_t liveShapeLimit = 32;
inline constexpr size_t shapesPerSecond = 10, messagesPerSecond = 50;
inline constexpr uint64_t stateIntervalUs = 25000;   // states coalesced, latest wins
inline constexpr uint64_t warpIntervalUs = 100000;   // at most 10 positions/s
inline constexpr uint64_t warpQuietUs = 50000;       // no injected input this long
inline constexpr int warpTolerancePixels = 2;        // absolute-input rounding
// The same handle is re-read this often: a pointer scheme change rewrites the
// bitmaps of the shared system cursors in place, under unchanged handles.
inline constexpr uint64_t revalidateUs = 1000000;
// cursor-echo-v1 (docs/remote-session-protocol.md "Cursor echo"): while the
// client holds the mouse (게임 마우스) it asks for the VM's real cursor position
// ({"type":"cursor.echo","on":true}); the worker reads it every echoPollUs
// and sends each new position at once, never two closer than echoMinimumUs
// (at most 240 a second on top of the 50 above; the poll right after the
// spacing sends the newest position, so the resting position always leaves).
// Single output only. A spacing of 1.5 ms (667/s) is too dense: every
// record is never-dropped control in the service's send queue (1024 packets),
// so a broker uplink stall of about 1.4 s closes the whole Host channel.
inline constexpr const char* echoProtocol = "cursor-echo-v1";
inline constexpr uint64_t echoPollUs = 2000, echoMinimumUs = 4167;
// cursor-compose-v1 (docs/remote-session-protocol.md "Cursor in the video"):
// while the client holds the mouse (게임 마우스) it asks for the VM's real cursor
// inside the video ({"type":"cursor.compose","on":true}) and shows no cursor of
// its own, so cursor and game arrive in the same frame (as in Moonlight). The
// capture then draws the cursor exactly as Windows shows it (nothing while an
// application hides it) and the client is told it is hidden, as for an
// unrepresentable shape. Single output only; it ends on off, with the session
// and with the worker.
inline constexpr const char* composeProtocol = "cursor-compose-v1";
// A position on an output of `real` pixels in the stream's announced source of
// `logicalWidth` x `logicalHeight` (capture_follow.hpp: an application changed
// the output's mode under a running stream; the client and the relay keep the
// announced size). The same size maps 1:1.
inline POINT logicalPosition(POINT offset, LONG realWidth, LONG realHeight, LONG logicalWidth, LONG logicalHeight) {
  if (realWidth <= 0 || realHeight <= 0 || logicalWidth <= 0 || logicalHeight <= 0 || (realWidth == logicalWidth && realHeight == logicalHeight)) return offset;
  const auto axis = [](LONG value, LONG real, LONG logical) { return LONG(std::clamp<int64_t>(int64_t(value) * logical / real, 0, int64_t(logical) - 1)); };
  return POINT{axis(offset.x, realWidth, logicalWidth), axis(offset.y, realHeight, logicalHeight)};
}

enum class Format : uint8_t { colour = 1, monochrome = 2 };
struct Shape {
  Format format = Format::colour;
  uint16_t width = 0, height = 0, hotspotX = 0, hotspotY = 0;
  // Mask rows of maskStride(width) bytes, top-down: height rows (colour) or the
  // AND rows followed by the XOR rows (monochrome). Bits past width are zero.
  std::vector<uint8_t> mask;
  // Colour only: width*height BGRA pixels, top-down, alpha as Windows stores it.
  std::vector<uint8_t> colour;
};
inline size_t maskStride(unsigned width) { return ((size_t(width) + 31) / 32) * 4; }
inline size_t maskRows(const Shape& shape) { return size_t(shape.height) * (shape.format == Format::monochrome ? 2 : 1); }
inline bool validShape(const Shape& shape) {
  if (!shape.width || !shape.height || shape.width > maximumSide || shape.height > maximumSide ||
      shape.hotspotX >= shape.width || shape.hotspotY >= shape.height) return false;
  if (shape.format != Format::colour && shape.format != Format::monochrome) return false;
  if (shape.mask.size() != maskStride(shape.width) * maskRows(shape)) return false;
  if (shape.colour.size() != (shape.format == Format::colour ? size_t(shape.width) * shape.height * 4 : 0)) return false;
  const size_t stride = maskStride(shape.width), used = (size_t(shape.width) + 7) / 8;
  for (size_t row = 0; row < maskRows(shape); ++row) {
    const auto* bytes = shape.mask.data() + row * stride;
    if (shape.width % 8 && (bytes[used - 1] & uint8_t(0xffu >> (shape.width % 8)))) return false;
    for (size_t i = used; i < stride; ++i) if (bytes[i]) return false;
  }
  return true;
}
// Content identity (FNV-1a 64): a reused handle or an in-place scheme change
// cannot keep showing an old shape under its id.
inline uint64_t shapeHash(const Shape& shape) {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto add = [&hash](const uint8_t* bytes, size_t count) { for (size_t i = 0; i < count; ++i) { hash ^= bytes[i]; hash *= 0x100000001b3ull; } };
  const uint16_t header[5] = {uint16_t(shape.format), shape.width, shape.height, shape.hotspotX, shape.hotspotY};
  add(reinterpret_cast<const uint8_t*>(header), sizeof(header));
  add(shape.mask.data(), shape.mask.size());
  add(shape.colour.data(), shape.colour.size());
  return hash;
}

namespace detail {
struct IconCopy { HICON value = nullptr; ~IconCopy() { if (value) DestroyIcon(value); } };
struct IconBitmaps { ICONINFO info{}; ~IconBitmaps() { if (info.hbmMask) DeleteObject(info.hbmMask); if (info.hbmColor) DeleteObject(info.hbmColor); } };
struct MemoryDc { HDC value = CreateCompatibleDC(nullptr); ~MemoryDc() { if (value) DeleteDC(value); } };
// Top-down rows of a DDB/DIB at 1 bpp (black/white palette) or 32 bpp.
inline bool readBits(HDC dc, HBITMAP bitmap, unsigned width, unsigned rows, bool monochrome, std::vector<uint8_t>& out) {
  struct Info { BITMAPINFOHEADER header{}; RGBQUAD colors[2]{}; } info;
  info.header.biSize = sizeof(BITMAPINFOHEADER); info.header.biWidth = LONG(width); info.header.biHeight = -LONG(rows);
  info.header.biPlanes = 1; info.header.biBitCount = WORD(monochrome ? 1 : 32); info.header.biCompression = BI_RGB;
  if (monochrome) { info.header.biClrUsed = 2; info.colors[1] = {255, 255, 255, 0}; }
  const size_t stride = monochrome ? maskStride(width) : size_t(width) * 4;
  out.assign(stride * rows, 0);
  if (GetDIBits(dc, bitmap, 0, rows, out.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS) != int(rows)) return false;
  if (monochrome) {
    const size_t used = (size_t(width) + 7) / 8;
    for (unsigned y = 0; y < rows; ++y) {
      auto* row = out.data() + size_t(y) * stride;
      if (width % 8) row[used - 1] &= uint8_t(0xffu << (8 - width % 8));
      std::fill(row + used, row + stride, uint8_t(0));
    }
  }
  return true;
}
}

// The cursor's own bitmaps in the native Windows format, or nullopt when it
// cannot be represented (null/destroyed handle, no 1-bpp mask, a side larger
// than 256 pixels): the capture then composes that cursor into the video as
// before. An animated cursor yields its first frame (GetIconInfo).
inline std::optional<Shape> extract(HCURSOR handle) {
  if (!handle) return std::nullopt;
  detail::IconCopy copy{CopyIcon(reinterpret_cast<HICON>(handle))};
  if (!copy.value) return std::nullopt;
  detail::IconBitmaps icon;
  if (!GetIconInfo(copy.value, &icon.info) || !icon.info.hbmMask) return std::nullopt;
  BITMAP mask{};
  if (GetObjectW(icon.info.hbmMask, sizeof(mask), &mask) != sizeof(mask) || mask.bmPlanes != 1 || mask.bmBitsPixel != 1 || mask.bmWidth <= 0 || mask.bmHeight <= 0) return std::nullopt;
  Shape shape;
  unsigned width = 0, height = 0;
  if (icon.info.hbmColor) {
    BITMAP colour{};
    if (GetObjectW(icon.info.hbmColor, sizeof(colour), &colour) != sizeof(colour) || colour.bmWidth != mask.bmWidth || colour.bmHeight != mask.bmHeight) return std::nullopt;
    width = unsigned(colour.bmWidth); height = unsigned(colour.bmHeight); shape.format = Format::colour;
  } else {
    if (mask.bmHeight % 2) return std::nullopt;
    width = unsigned(mask.bmWidth); height = unsigned(mask.bmHeight / 2); shape.format = Format::monochrome;
  }
  if (width > maximumSide || height > maximumSide || icon.info.xHotspot >= width || icon.info.yHotspot >= height) return std::nullopt;
  shape.width = uint16_t(width); shape.height = uint16_t(height);
  shape.hotspotX = uint16_t(icon.info.xHotspot); shape.hotspotY = uint16_t(icon.info.yHotspot);
  detail::MemoryDc dc;
  if (!dc.value || !detail::readBits(dc.value, icon.info.hbmMask, width, unsigned(maskRows(shape)), true, shape.mask)) return std::nullopt;
  if (shape.format == Format::colour && !detail::readBits(dc.value, icon.info.hbmColor, width, height, false, shape.colour)) return std::nullopt;
  if (!validShape(shape)) return std::nullopt;
  return shape;
}

// ---- The size Windows shows -------------------------------------------------
// extract() reads a cursor's own bitmap. That is not always the size on the
// screen: Windows keeps one bitmap per cursor and scales it for the monitor it
// is shown on, by monitor DPI / the DPI the cursor was made for
// (SetThreadCursorCreationScaling). The system cursors are loaded once, at the
// DPI the session had then. A desktop that signed in at 100 % and is streamed
// at 150 % (display-scale-v1 changes the output's scale while it runs) shows a
// 48-pixel pointer whose bitmap is still 32 pixels: the client drew that 32
// pixel shape as so many picture pixels, two thirds of the right size (half at
// 200 %). The shape sent is now the one of the size Windows shows, so every
// client is right without a protocol change (a shape pixel stays one source
// pixel).
//
// How large Windows shows a cursor is known for
//   - the system cursors (arrow, I-beam, hand, resize, wait ...): the nominal
//     cursor size of the output's DPI (32 at 100 %, 48 at 150 %, 64 at 200 %),
//     whatever size their bitmap has. Measured against the system arrow's own
//     bitmap, so a session whose bitmaps already have that size is left alone;
//   - a cursor of a window Windows itself scales (DPI unaware, or system DPI
//     aware with another system DPI): by output DPI / that DPI.
// A program that is per-monitor DPI aware sizes its own cursors; those are
// sent as they are (unknown is never scaled).
struct ShownInputs {
  unsigned outputDpi = 96;      // effective DPI of the output under the pointer
  bool systemCursor = false;    // one of the shared system cursors
  unsigned arrowSide = 0;       // width of the system arrow's own bitmap
  unsigned nominalSide = 0;     // SM_CXCURSOR for outputDpi
  // The window under the pointer: 0 DPI unaware, 1 system DPI aware, 2 per
  // monitor, -1 unknown; and its process' system DPI (0 unknown).
  int awareness = -1;
  unsigned processDpi = 0;
};
// Differences of at most 5 % are not a different size (a bitmap is never
// resampled for them), as on the client (ScaledCursorSize).
inline double shownFactor(const ShownInputs& in) {
  double factor = 1.0;
  if (in.outputDpi < 48 || in.outputDpi > 960) return 1.0;
  if (in.systemCursor) { if (in.arrowSide && in.nominalSide) factor = double(in.nominalSide) / double(in.arrowSide); }
  else if (in.awareness == 0) factor = double(in.outputDpi) / 96.0;
  else if (in.awareness == 1 && in.processDpi >= 48 && in.processDpi <= 960) factor = double(in.outputDpi) / double(in.processDpi);
  if (!(factor > 0) || factor != factor || (factor >= 0.95 && factor <= 1.05)) return 1.0;
  return std::clamp(factor, 0.25, 8.0);
}
// Both sides by one factor, never past the largest shape the channel carries.
inline SIZE shownSize(unsigned width, unsigned height, double factor) {
  if (!width || !height || !(factor > 0)) return SIZE{LONG(width), LONG(height)};
  factor = std::min(factor, double(maximumSide) / double(std::max(width, height)));
  const auto side = [&](unsigned value) { return LONG(std::clamp<long long>(static_cast<long long>(double(value) * factor + 0.5), 1, maximumSide)); };
  return SIZE{side(width), side(height)};
}
// The cursor's shape at `factor` times its own bitmap, made by Windows itself
// (CopyImage reloads a resource cursor at the new size when its file has one,
// as Windows does for the screen, and stretches otherwise). The unscaled shape
// when Windows cannot make it.
inline std::optional<Shape> extractScaled(HCURSOR handle, double factor) {
  auto base = extract(handle);
  if (!base) return std::nullopt;
  const SIZE size = shownSize(base->width, base->height, factor);
  if (size.cx == LONG(base->width) && size.cy == LONG(base->height)) return base;
  detail::IconCopy scaled{reinterpret_cast<HICON>(CopyImage(handle, IMAGE_CURSOR, int(size.cx), int(size.cy), LR_COPYFROMRESOURCE))};
  if (!scaled.value) scaled.value = reinterpret_cast<HICON>(CopyImage(handle, IMAGE_CURSOR, int(size.cx), int(size.cy), 0));
  if (!scaled.value) return base;
  auto shape = extract(reinterpret_cast<HCURSOR>(scaled.value));
  if (!shape || shape->format != base->format || LONG(shape->width) != size.cx || LONG(shape->height) != size.cy) return base;
  return shape;
}
namespace detail {
inline bool systemCursor(HCURSOR handle) {
  // The shared handles of the session's cursor scheme (winuser.h OCR_* ids).
  for (const int id : {32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32650, 32651, 32671, 32672})
    if (handle == LoadCursorW(nullptr, MAKEINTRESOURCEW(id))) return true;
  return false;
}
inline unsigned monitorDpi(POINT position) {
  // shcore is not linked by every target that includes this header.
  using Get = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static const Get get = [] { const HMODULE module = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); return module ? reinterpret_cast<Get>(GetProcAddress(module, "GetDpiForMonitor")) : nullptr; }();
  UINT x = 0, y = 0;
  const HMONITOR monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
  return get && monitor && SUCCEEDED(get(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) ? unsigned(x) : 0;
}
// The window under the pointer set this cursor (WM_SETCURSOR).
inline void windowScaling(POINT position, ShownInputs& in) {
  const HWND window = WindowFromPoint(position);
  if (!window) return;
  in.awareness = int(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window)));
  if (in.awareness != 1) return;
  using Get = UINT(WINAPI*)(HANDLE);  // Windows 10 1803
  static const Get get = reinterpret_cast<Get>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemDpiForProcess"));
  DWORD pid = 0;
  if (!get || !GetWindowThreadProcessId(window, &pid) || !pid) return;
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return;
  in.processDpi = get(process);
  CloseHandle(process);
}
}
// What the size decision needs, read from Windows for the cursor now showing.
inline ShownInputs readShownInputs(HCURSOR handle) {
  ShownInputs in;
  CURSORINFO info{sizeof(CURSORINFO)};
  if (!handle || !GetCursorInfo(&info)) return in;
  in.outputDpi = detail::monitorDpi(info.ptScreenPos);
  if (!in.outputDpi) { in.outputDpi = 96; return in; }
  in.systemCursor = detail::systemCursor(handle);
  if (in.systemCursor) {
    if (const auto arrow = extract(LoadCursorW(nullptr, IDC_ARROW))) in.arrowSide = arrow->width;
    in.nominalSide = unsigned(std::max(0, GetSystemMetricsForDpi(SM_CXCURSOR, in.outputDpi)));
  } else detail::windowScaling(info.ptScreenPos, in);
  return in;
}
// The channel's extractor: the shape at the size Windows shows it.
inline std::optional<Shape> extractShown(HCURSOR handle) {
  const double factor = shownFactor(readShownInputs(handle));
  return factor == 1.0 ? extract(handle) : extractScaled(handle, factor);
}

inline void put16(std::vector<uint8_t>& out, size_t at, uint16_t value) { out[at] = uint8_t(value); out[at + 1] = uint8_t(value >> 8); }
inline void put32(std::vector<uint8_t>& out, size_t at, uint32_t value) { for (int i = 0; i < 4; ++i) out[at + i] = uint8_t(value >> (8 * i)); }
// Record payloads (the 12-byte media header is added by mediaPacket(3, ...)).
inline std::vector<uint8_t> shapeRecord(const Shape& shape, uint32_t id) {
  if (!validShape(shape) || !id || id > maximumShapeId) throw std::runtime_error("Invalid cursor shape record");
  std::vector<uint8_t> out(16 + shape.mask.size() + shape.colour.size(), 0);
  out[0] = shapeRecordType; out[1] = uint8_t(shape.format);
  put32(out, 4, id); put16(out, 8, shape.width); put16(out, 10, shape.height); put16(out, 12, shape.hotspotX); put16(out, 14, shape.hotspotY);
  std::memcpy(out.data() + 16, shape.mask.data(), shape.mask.size());
  if (!shape.colour.empty()) std::memcpy(out.data() + 16 + shape.mask.size(), shape.colour.data(), shape.colour.size());
  return out;
}
inline std::vector<uint8_t> stateRecord(bool visible, uint32_t id, const std::optional<POINT>& position) {
  if (visible ? (!id || id > maximumShapeId) : id != 0) throw std::runtime_error("Invalid cursor state record");
  if (position && (position->x < 0 || position->y < 0 || position->x > 16383 || position->y > 16383)) throw std::runtime_error("Invalid cursor position");
  std::vector<uint8_t> out(16, 0);
  out[0] = stateRecordType; out[1] = uint8_t((visible ? 1 : 0) | (position ? 2 : 0));
  put32(out, 4, id);
  if (position) { put32(out, 8, uint32_t(position->x)); put32(out, 12, uint32_t(position->y)); }
  return out;
}

// session.start of a session whose broker granted cursor-v1 (only a granting
// broker sends the marker; RDP never carries it, tracks-v1 only with
// cursor-tracks-v1 below).
template<class JsonValue> bool sessionRequested(const JsonValue& start) {
  return start.get("type").string() == "session.start" && start.get("mode").string() != "rdp" &&
         start.get("cursorProtocol").string() == protocol && start.get("mediaProtocol").string().empty();
}
// session.start of independent outputs whose broker granted both markers.
template<class JsonValue> bool trackSessionRequested(const JsonValue& start) {
  return start.get("type").string() == "session.start" && start.get("mode").string() == "stream" &&
         start.get("mediaProtocol").string() == "tracks-v1" && start.get("cursorProtocol").string() == protocol &&
         start.get("cursorTracksProtocol").string() == tracksProtocol;
}
// cursor-echo-v1: an ordinary stream session whose broker granted it together
// with cursor-v1 (never independent outputs, never RDP).
template<class JsonValue> bool echoSessionRequested(const JsonValue& start) {
  return sessionRequested(start) && start.get("mode").string() == "stream" &&
         start.get("cursorEchoProtocol").string() == echoProtocol;
}
// {"type":"cursor.echo","on":bool[,"sessionId":...]}; nothing else.
template<class JsonValue> std::optional<bool> echoRequest(const JsonValue& message) {
  const auto* fields = std::get_if<typename JsonValue::Object>(&message.value);
  if (!fields || message.get("type").string() != "cursor.echo") return std::nullopt;
  for (const auto& [key, item] : *fields) { (void)item; if (key != "type" && key != "on" && key != "sessionId") return std::nullopt; }
  const auto* on = std::get_if<bool>(&message.get("on").value);
  if (!on) return std::nullopt;
  return *on;
}
// cursor-compose-v1: an ordinary stream session whose broker granted it
// together with cursor-v1 (never independent outputs, never RDP).
template<class JsonValue> bool composeSessionRequested(const JsonValue& start) {
  return sessionRequested(start) && start.get("mode").string() == "stream" &&
         start.get("cursorComposeProtocol").string() == composeProtocol;
}
// cursor-compose-v1 on independent outputs (게임 마우스 in 모든 모니터): a
// cursor-tracks-v1 session whose broker granted it too; the output the cursor
// is over composes it.
template<class JsonValue> bool composeTrackSessionRequested(const JsonValue& start) {
  return trackSessionRequested(start) && start.get("cursorComposeProtocol").string() == composeProtocol;
}
// {"type":"cursor.compose","on":bool[,"sessionId":...]}; nothing else.
template<class JsonValue> std::optional<bool> composeRequest(const JsonValue& message) {
  const auto* fields = std::get_if<typename JsonValue::Object>(&message.value);
  if (!fields || message.get("type").string() != "cursor.compose") return std::nullopt;
  for (const auto& [key, item] : *fields) { (void)item; if (key != "type" && key != "on" && key != "sessionId") return std::nullopt; }
  const auto* on = std::get_if<bool>(&message.get("on").value);
  if (!on) return std::nullopt;
  return *on;
}
// One worker Agent's cursor-compose-v1 request: granted per session.start; the
// client's latest on/off, kept across the stream restarts of a resize of the
// same session (like the echo), never into another or an ungranted session.
// Read by the capture thread on every tick.
class ComposeRequest {
 public:
  void configure(bool granted, const std::string& session) {
    std::lock_guard lock(mutex_);
    granted_ = granted;
    if (!granted || session != session_) wanted_ = false;
    session_ = granted ? session : std::string();
  }
  // The client's request; ignored (false) without the grant.
  bool set(bool on) { std::lock_guard lock(mutex_); if (!granted_) return false; wanted_ = on; return true; }
  void end() { std::lock_guard lock(mutex_); wanted_ = false; session_.clear(); }
  bool granted() const { return granted_.load(); }
  bool wanted() const { return wanted_.load(); }
 private:
  std::mutex mutex_;
  std::atomic<bool> granted_{false}, wanted_{false};
  std::string session_;
};

// Shape ids strictly increase within one session, across capture epochs (a
// resize restart re-sends the current shape under a new id). 0 = exhausted.
class SessionShapeIds {
 public:
  uint32_t next(const std::string& session) {
    std::lock_guard lock(mutex_);
    if (session != session_) { session_ = session; last_ = 0; }
    return last_ >= maximumShapeId ? 0 : ++last_;
  }
 private:
  std::mutex mutex_; std::string session_; uint32_t last_ = 0;
};

// Where our last injected pointer event put the cursor (screen pixels), written
// by the control thread after SendInput succeeded, read by the capture thread.
class InjectedPointer {
 public:
  struct Mark { bool known = false; POINT position{}; uint64_t atUs = 0; uint64_t serial = 0; };
  void injected(POINT position, uint64_t atUs) { std::lock_guard lock(mutex_); mark_.known = true; mark_.position = position; mark_.atUs = atUs; ++mark_.serial; }
  Mark read() const { std::lock_guard lock(mutex_); return mark_; }
  void reset() { std::lock_guard lock(mutex_); mark_ = {}; }
 private:
  mutable std::mutex mutex_; Mark mark_;
};

// One GetCursorInfo result of a capture tick (screen pixels).
struct Observation { bool known = false; DWORD flags = 0; HCURSOR handle = nullptr; POINT position{}; };

// One capture epoch of one granted session; capture thread only.
class Channel {
 public:
  // Production reads the shape at the size Windows shows it (extractShown).
  using Extract = std::function<std::optional<Shape>(HCURSOR)>;
  using Allocate = std::function<uint32_t()>;
  struct Counters { uint64_t shapes = 0, states = 0, warps = 0, composed = 0, echoes = 0; };
  Channel(RECT source, Allocate allocate, Extract extractor = extractShown)
      : Channel(std::vector<RECT>{source}, true, std::move(allocate), std::move(extractor)) {}
  // cursor-tracks-v1: the owned outputs of independent outputs (one or two).
  // Visible means the hotspot is inside any of them. No position is ever sent:
  // the client's pointer already is over the output it drives.
  Channel(const std::vector<RECT>& outputs, Allocate allocate, Extract extractor = extractShown)
      : Channel(outputs, false, std::move(allocate), std::move(extractor)) {}
  // The epoch's first video key frame was queued: records may follow it.
  void keyFrame() { ready_ = true; }
  bool ready() const { return ready_; }
  // Whether the capture must compose the current cursor into the video: it
  // cannot be represented as a record, so it is reported hidden instead.
  bool composeFallback() const { return compose_; }
  // cursor-compose-v1: while asked, every tick composes (the capture draws the
  // cursor as Windows shows it) and the client is told hidden; shapes wait.
  // Positions (warps at rest) are still sent. Off: the next tick reports the
  // real state again and composition follows only the fallback.
  void composeRequested(bool on) { requested_ = on; }
  // The stream restarted its encoder inside this epoch (stream-size-v1: a new
  // stream.info, then an IDR). The client starts a new decoder for it: the
  // Windows runner under a new media lease, which empties its cursor cache,
  // and every client drops records until that IDR and shows its local pointer
  // until a state arrives for the new decoder. Nothing the client was sent
  // counts any more: records wait for the key frame again, then each shape is
  // sent anew under a new id, followed by the current state. If the
  // channel went on naming the forgotten ids, the client would reject every such
  // state and show its local arrow instead (a window edge's resize cursor
  // "works, then does not").
  void pictureRestart() {
    ready_ = false; sent_.reset(); entries_.clear();
    memoHandle_ = nullptr; memoUs_.reset(); memoId_ = 0;
    echoed_.reset();
  }
  Counters takeCounters() { const auto value = counters_; counters_ = {}; return value; }
  size_t liveShapes() const { return entries_.size(); }

  // The picture of this tick is not the capture's: the SYSTEM helper's frame of
  // a secure desktop (secure_desktop_switch.hpp), which has that desktop's
  // pointer composed into it. The client is told hidden (once, within the same
  // bounds as any state), so it does not draw its own pointer over the composed
  // one. No shape, position or baseline changes: the first observe() after the
  // desktop returns reports the real cursor again.
  std::vector<std::vector<uint8_t>> hide(uint64_t nowUs) {
    std::vector<std::vector<uint8_t>> records;
    trim(nowUs);
    const State hidden{false, 0};
    previous_.reset();
    if (ready_ && (!sent_ || *sent_ != hidden) && (!lastStateUs_ || nowUs - *lastStateUs_ >= stateIntervalUs) && messageTimes_.size() < messagesPerSecond) {
      records.push_back(stateRecord(false, 0, std::nullopt));
      sent_ = hidden; lastStateUs_ = nowUs; messageTimes_.push_back(nowUs); ++counters_.states;
    }
    return records;
  }

  // One capture tick. Returns the record payloads to send now, in order.
  std::vector<std::vector<uint8_t>> observe(const Observation& seen, const InjectedPointer::Mark& mark, uint64_t nowUs) {
    std::vector<std::vector<uint8_t>> records;
    trim(nowUs);
    // Failed observation says nothing about visibility. Keep the previous
    // state (and composition choice) until an actual Windows observation is
    // available; old cursor-v1 peers need no new flag or protocol change.
    if (!seen.known) { previous_.reset(); return records; }
    const bool showing = seen.known && (seen.flags & CURSOR_SHOWING) && !(seen.flags & CURSOR_SUPPRESSED) && seen.handle;
    const bool inside = seen.known && contains(seen.position);
    const bool candidate = showing && inside;
    Entry* entry = nullptr; bool unsupported = false;
    if (candidate && !requested_) {
      entry = resolve(seen.handle, nowUs);
      unsupported = !entry;
    }
    const bool shapeBudget = ready_ && shapeTimes_.size() < shapesPerSecond && messageTimes_.size() < messagesPerSecond;
    // Resolution can reserve ids while the send budget is exhausted. If a
    // newer deferred shape was published first, renew this still-unsent id
    // before publication; cached states keep their already-published ids.
    if (entry && !entry->sent && shapeBudget && entry->id <= lastPublishedShape_) {
      const uint32_t id = allocate_();
      if (!id || id > maximumShapeId || id <= lastPublishedShape_) { entry = nullptr; unsupported = true; }
      else { entry->id = id; memoId_ = id; }
    }
    compose_ = requested_ || (candidate && unsupported);
    if (candidate && unsupported) ++counters_.composed;
    // A new shape is sent first; a state never names an unsent id.
    if (entry && !entry->sent && shapeBudget) {
      records.push_back(shapeRecord(entry->shape, entry->id));
      lastPublishedShape_ = entry->id;
      entry->sent = true; entry->shape = {}; shapeTimes_.push_back(nowUs); messageTimes_.push_back(nowUs); ++counters_.shapes;
    }
    std::optional<State> desired;
    if (requested_ || !candidate || unsupported) desired = State{false, 0};
    else if (entry->sent) desired = State{true, entry->id};
    // else: the shape waits for its budget; keep the state the client has.
    // Position: only for movement our injected input did not cause.
    std::optional<POINT> warp;
    if (seen.known && warps_) {
      if (mark.known && mark.serial != markSerial_) { expected_ = mark.position; markSerial_ = mark.serial; }
      if (!expected_) expected_ = seen.position;  // baseline: no injection yet
      const bool stable = previous_ && previous_->x == seen.position.x && previous_->y == seen.position.y;
      const bool moved = std::max(std::abs(seen.position.x - expected_->x), std::abs(seen.position.y - expected_->y)) > warpTolerancePixels;
      const bool quiet = !mark.known || (nowUs >= mark.atUs && nowUs - mark.atUs >= warpQuietUs);
      if (ready_ && inside && stable && moved && quiet && (!lastWarpUs_ || nowUs - *lastWarpUs_ >= warpIntervalUs))
        warp = outputPosition(seen.position);
      previous_ = seen.position;
    } else previous_.reset();
    const State next = desired ? *desired : (sent_ ? *sent_ : State{false, 0});
    const bool changed = desired && (!sent_ || *sent_ != *desired);
    if (ready_ && (changed || warp) && (!lastStateUs_ || nowUs - *lastStateUs_ >= stateIntervalUs) && messageTimes_.size() < messagesPerSecond) {
      records.push_back(stateRecord(next.visible, next.id, warp));
      sent_ = next; lastStateUs_ = nowUs; messageTimes_.push_back(nowUs); ++counters_.states;
      if (warp) { expected_ = seen.position; echoed_ = *warp; lastWarpUs_ = nowUs; ++counters_.warps; }
    }
    return records;
  }

  // cursor-echo-v1: one poll while the client holds the mouse. A position
  // the client was not sent yet becomes a state of the last sent visibility
  // and shape with that position, at once: no stillness, quiet period or
  // 10/s bound, and outside the 50/s budget of the records above. Nothing
  // before the epoch's first key frame and first state, outside the output,
  // for independent outputs, or within echoMinimumUs of the previous echo
  // (the next poll sends the newest position instead).
  std::optional<std::vector<uint8_t>> echo(const Observation& seen, uint64_t nowUs) {
    if (!warps_ || !ready_ || !sent_ || !seen.known || !contains(seen.position)) return std::nullopt;
    const POINT position = outputPosition(seen.position);
    if (echoed_ && echoed_->x == position.x && echoed_->y == position.y) return std::nullopt;
    if (lastEchoUs_ && nowUs >= *lastEchoUs_ && nowUs - *lastEchoUs_ < echoMinimumUs) return std::nullopt;
    echoed_ = position; lastEchoUs_ = nowUs;
    // The regular path must not report this position again as a warp.
    expected_ = seen.position;
    ++counters_.echoes;
    return stateRecord(sent_->visible, sent_->id, position);
  }
  // A new echo request: its first poll sends the position even if unchanged.
  void echoRestart() { echoed_.reset(); }
  // Single output only: the output is now `area` (screen pixels) while the
  // stream keeps announcing `logicalWidth` x `logicalHeight`; later positions
  // are scaled into that size. Shapes, ids and the sent state are kept; the
  // next echo sends the current position again.
  void rebase(RECT area, LONG logicalWidth, LONG logicalHeight) {
    if (!warps_ || logicalWidth < 1 || logicalHeight < 1 || !checkedFrameLayout(uint64_t(int64_t(area.right) - area.left), uint64_t(int64_t(area.bottom) - area.top), false)) throw std::runtime_error("Invalid cursor channel");
    areas_ = {area}; logicalWidth_ = logicalWidth; logicalHeight_ = logicalHeight;
    expected_.reset(); previous_.reset(); echoed_.reset();
  }
  // cursor-tracks-v1: output `index` (in the constructor's order) now reads
  // `area` (capture_follow.hpp); the cursor is visible over it there.
  void moveOutput(size_t index, RECT area) {
    if (warps_ || index >= areas_.size() || !checkedFrameLayout(uint64_t(int64_t(area.right) - area.left), uint64_t(int64_t(area.bottom) - area.top), false)) throw std::runtime_error("Invalid cursor channel");
    areas_[index] = area;
  }

 private:
  POINT outputPosition(POINT screen) const {
    const RECT& area = areas_.front();
    return logicalPosition(POINT{screen.x - area.left, screen.y - area.top}, area.right - area.left, area.bottom - area.top, logicalWidth_, logicalHeight_);
  }
  struct State { bool visible = false; uint32_t id = 0; friend bool operator==(const State&, const State&) = default; };
  struct Entry { HCURSOR handle = nullptr; uint64_t hash = 0; uint32_t id = 0; bool sent = false; uint64_t usedUs = 0; Shape shape; };
  Channel(std::vector<RECT> areas, bool warps, Allocate allocate, Extract extractor)
      : areas_(std::move(areas)), warps_(warps), allocate_(std::move(allocate)), extract_(std::move(extractor)) {
    if (areas_.empty() || areas_.size() > 2 || !allocate_ || !extract_) throw std::runtime_error("Invalid cursor channel");
    for (const auto& area : areas_)
      if (!checkedFrameLayout(uint64_t(int64_t(area.right)-area.left),uint64_t(int64_t(area.bottom)-area.top),false)) throw std::runtime_error("Invalid cursor channel");
  }
  bool contains(POINT point) const {
    return std::any_of(areas_.begin(), areas_.end(), [point](const RECT& area) { return point.x >= area.left && point.x < area.right && point.y >= area.top && point.y < area.bottom; });
  }
  void trim(uint64_t nowUs) {
    for (auto* times : {&shapeTimes_, &messageTimes_})
      while (!times->empty() && (nowUs < times->front() || nowUs - times->front() >= 1000000)) times->pop_front();
  }
  Entry* find(uint32_t id) { for (auto& entry : entries_) if (entry.id == id) return &entry; return nullptr; }
  // The cached entry for the current handle, or nullptr when the cursor
  // cannot be represented. Extraction runs only for a new handle and once per
  // revalidation period for an unchanged one.
  Entry* resolve(HCURSOR handle, uint64_t nowUs) {
    if (handle == memoHandle_ && memoUs_ && nowUs >= *memoUs_ && nowUs - *memoUs_ < revalidateUs) {
      if (!memoId_) return nullptr;
      if (auto* entry = find(memoId_)) { entry->usedUs = nowUs; return entry; }
    }
    memoHandle_ = handle; memoUs_ = nowUs; memoId_ = 0;
    auto shape = extract_(handle);
    if (!shape || !validShape(*shape)) return nullptr;
    const auto hash = shapeHash(*shape);
    for (auto& entry : entries_)
      if (entry.hash == hash) { entry.handle = handle; entry.usedUs = nowUs; memoId_ = entry.id; return &entry; }
    const uint32_t id = allocate_();
    if (!id || id > maximumShapeId) return nullptr;
    // Evict the least recently used shape the client may still cache; the
    // client keeps more shapes than the host names, so no state can name an
    // id the client already dropped.
    while (entries_.size() >= liveShapeLimit) {
      auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.usedUs < b.usedUs; });
      entries_.erase(oldest);
    }
    entries_.push_back({handle, hash, id, false, nowUs, std::move(*shape)});
    memoId_ = id;
    return &entries_.back();
  }
  std::vector<RECT> areas_;
  LONG logicalWidth_ = 0, logicalHeight_ = 0;  // 0: positions in output pixels
  bool warps_ = true;
  Allocate allocate_;
  Extract extract_;
  std::vector<Entry> entries_;
  uint32_t lastPublishedShape_ = 0;
  HCURSOR memoHandle_ = nullptr; std::optional<uint64_t> memoUs_; uint32_t memoId_ = 0;
  std::optional<State> sent_;
  std::optional<uint64_t> lastStateUs_, lastWarpUs_, lastEchoUs_;
  std::optional<POINT> expected_, previous_, echoed_;
  uint64_t markSerial_ = 0;
  std::deque<uint64_t> shapeTimes_, messageTimes_;
  bool ready_ = false, compose_ = false, requested_ = false;
  Counters counters_;
};

// A channel the capture thread shares with the cursor-echo-v1 poller. Every
// use, including handing its records on, happens under one lock, so a state
// never leaves before the shape it names.
class SharedChannel {
 public:
  using Emit = std::function<void(const std::vector<uint8_t>&)>;
  template<class... Arguments> explicit SharedChannel(Arguments&&... arguments) : channel_(std::forward<Arguments>(arguments)...) {}
  void observe(const Observation& seen, const InjectedPointer::Mark& mark, uint64_t nowUs, const Emit& emit) {
    std::lock_guard lock(mutex_);
    for (const auto& record : channel_.observe(seen, mark, nowUs)) emit(record);
  }
  // fresh: the first poll of a new echo request.
  void echo(const Observation& seen, bool fresh, uint64_t nowUs, const Emit& emit) {
    std::lock_guard lock(mutex_);
    if (fresh) channel_.echoRestart();
    if (const auto record = channel_.echo(seen, nowUs)) emit(*record);
  }
  void hide(uint64_t nowUs, const Emit& emit) {
    std::lock_guard lock(mutex_);
    for (const auto& record : channel_.hide(nowUs)) emit(record);
  }
  void keyFrame() { std::lock_guard lock(mutex_); channel_.keyFrame(); }
  void pictureRestart() { std::lock_guard lock(mutex_); channel_.pictureRestart(); }
  void rebase(RECT area, LONG logicalWidth, LONG logicalHeight) { std::lock_guard lock(mutex_); channel_.rebase(area, logicalWidth, logicalHeight); }
  bool composeFallback() { std::lock_guard lock(mutex_); return channel_.composeFallback(); }
  void composeRequested(bool on) { std::lock_guard lock(mutex_); channel_.composeRequested(on); }
  Channel::Counters takeCounters() { std::lock_guard lock(mutex_); return channel_.takeCounters(); }
 private:
  std::mutex mutex_;
  Channel channel_;
};

// The cursor as Windows reports it now (screen pixels); unknown on failure.
inline Observation readObservation() {
  CURSORINFO info{sizeof(CURSORINFO)};
  if (!GetCursorInfo(&info)) return {};
  return {true, info.flags, info.hCursor, info.ptScreenPos};
}

// cursor-echo-v1 poller of one capture epoch. `request` is 0 while the client
// does not ask, otherwise the number of its current request (a new number for
// every request). While asked, it reads the cursor every `intervalUs` on its
// own thread (a high-resolution timer) and hands each reading to `tick`, with
// fresh set on the first reading of a request. Unasked it reads nothing and
// waits on `wake`, which the owner signals whenever `request` changes. It is
// destroyed (and joined) before anything `tick` uses.
class EchoPoller {
 public:
  using Read = std::function<Observation()>;
  using Tick = std::function<void(const Observation&, bool fresh)>;
  EchoPoller(const std::atomic<uint32_t>& request, HANDLE wake, Read read, Tick tick, uint64_t intervalUs = echoPollUs)
      : request_(request), wake_(wake), read_(std::move(read)), tick_(std::move(tick)), intervalUs_(std::max<uint64_t>(intervalUs, 500)) {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_ || !read_ || !tick_) { if (stop_) CloseHandle(stop_); throw std::runtime_error("Invalid cursor echo"); }
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    try { thread_ = std::thread([this] { run(); }); }
    catch (...) { CloseHandle(stop_); if (timer_) CloseHandle(timer_); throw; }
  }
  ~EchoPoller() { SetEvent(stop_); if (thread_.joinable()) thread_.join(); CloseHandle(stop_); if (timer_) CloseHandle(timer_); }
  EchoPoller(const EchoPoller&) = delete;
  EchoPoller& operator=(const EchoPoller&) = delete;
  uint64_t polls() const { return polls_.load(); }
 private:
  bool stopping() const { return WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0; }
  void run() {
    uint32_t current = 0;
    while (!stopping()) {
      const uint32_t asked = request_.load();
      if (!asked) {
        current = 0;
        const HANDLE handles[2] = {stop_, wake_};
        WaitForMultipleObjects(wake_ ? 2 : 1, handles, FALSE, 100);
        continue;
      }
      const bool fresh = asked != current; current = asked;
      try { tick_(read_(), fresh); } catch (...) { }
      ++polls_;
      pause();
    }
  }
  void pause() {
    if (timer_) {
      LARGE_INTEGER due{}; due.QuadPart = -LONGLONG(intervalUs_ * 10);
      if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
        const HANDLE handles[2] = {stop_, timer_};
        WaitForMultipleObjects(2, handles, FALSE, 100);
        return;
      }
    }
    WaitForSingleObject(stop_, DWORD(std::max<uint64_t>(1, intervalUs_ / 1000)));
  }
  const std::atomic<uint32_t>& request_;
  HANDLE wake_ = nullptr, stop_ = nullptr, timer_ = nullptr;
  Read read_;
  Tick tick_;
  uint64_t intervalUs_;
  std::atomic<uint64_t> polls_{0};
  std::thread thread_;
};
}
