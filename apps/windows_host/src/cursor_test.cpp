// cursor-v1 on the host (docs/remote-session-protocol.md "Candidate cursor
// separation"): native-format conversion of every cursor kind (cursors created
// in memory plus the shared system cursors; the desktop cursor is never moved
// or read), the wire records, the per-epoch channel (cache, ids, visibility,
// fallback, rate bounds, position warps), the production composition switch
// with FrameEncodePolicy, the actual worker Agent input path, and a synthetic
// CPU measurement through the production cursor patch, encode policy and the
// Microsoft H.264 MFT. No network, VM or display setting is involved.
#include "agent.hpp"
#include "cursor_patch.hpp"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }

struct Bitmap { HBITMAP value = nullptr; ~Bitmap() { if (value) DeleteObject(value); } };
struct OwnedCursor {
  HCURSOR value = nullptr;
  explicit OwnedCursor(HCURSOR handle) : value(handle) {}
  OwnedCursor(OwnedCursor&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  OwnedCursor(const OwnedCursor&) = delete;
  ~OwnedCursor() { if (value) DestroyIcon(reinterpret_cast<HICON>(value)); }
};
HBITMAP dib(unsigned width, unsigned height, bool mono, const std::vector<uint8_t>& bytes) {
  struct Info { BITMAPINFOHEADER header{}; RGBQUAD colors[2]{}; } info;
  info.header.biSize = sizeof(BITMAPINFOHEADER); info.header.biWidth = LONG(width); info.header.biHeight = -LONG(height);
  info.header.biPlanes = 1; info.header.biBitCount = WORD(mono ? 1 : 32); info.header.biCompression = BI_RGB;
  if (mono) { info.header.biClrUsed = 2; info.colors[1] = {255, 255, 255, 0}; }
  void* pixels = nullptr;
  HBITMAP result = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &pixels, nullptr, 0);
  require(result && pixels, "Fixture bitmap creation failed");
  std::memcpy(pixels, bytes.data(), bytes.size());
  return result;
}
void setBit(std::vector<uint8_t>& data, unsigned width, unsigned y, unsigned x, bool set) { if (set) data[size_t(y) * cursor::maskStride(width) + x / 8] |= uint8_t(0x80u >> (x % 8)); }
bool bit(const std::vector<uint8_t>& data, unsigned width, unsigned y, unsigned x) { return (data[size_t(y) * cursor::maskStride(width) + x / 8] >> (7 - x % 8)) & 1; }
OwnedCursor make(const std::vector<uint8_t>& mask, const std::vector<uint8_t>& colour, unsigned width, unsigned height, unsigned hotX, unsigned hotY) {
  const bool mono = colour.empty();
  Bitmap maskBitmap{dib(width, height * (mono ? 2 : 1), true, mask)}, colourBitmap;
  if (!mono) colourBitmap.value = dib(width, height, false, colour);
  ICONINFO info{}; info.fIcon = FALSE; info.xHotspot = hotX; info.yHotspot = hotY; info.hbmMask = maskBitmap.value; info.hbmColor = colourBitmap.value;
  OwnedCursor result{reinterpret_cast<HCURSOR>(CreateIconIndirect(&info))};
  require(result.value != nullptr, "Fixture cursor creation failed");
  return result;
}
enum class Kind { alpha, masked, mono };
struct Fixture { Kind kind; unsigned width, height; std::vector<uint8_t> mask, colour; };
Fixture fixture(Kind kind, unsigned width, unsigned height) {
  Fixture value{kind, width, height, std::vector<uint8_t>(cursor::maskStride(width) * height * (kind == Kind::mono ? 2 : 1), 0), {}};
  if (kind != Kind::mono) value.colour.assign(size_t(width) * height * 4, 0);
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
    const size_t at = (size_t(y) * width + x) * 4;
    if (kind == Kind::alpha) {
      // Premultiplied colour with every alpha level class, AND mask clear.
      const unsigned alpha = ((x / 3 + y / 3) % 4) * 85;
      value.colour[at] = uint8_t(180 * alpha / 255); value.colour[at + 1] = uint8_t(90 * alpha / 255); value.colour[at + 2] = uint8_t(220 * alpha / 255); value.colour[at + 3] = uint8_t(alpha);
    } else if (kind == Kind::masked) {
      // No alpha: AND 1 keeps the screen (and XORs a non-black colour), AND 0 paints.
      value.colour[at] = uint8_t(x % 2 ? 255 : 0); value.colour[at + 1] = uint8_t(y % 2 ? 127 : 0); value.colour[at + 2] = uint8_t((x + y) % 2 ? 255 : 0);
      setBit(value.mask, width, y, x, ((x / 3 + y / 3) % 3) != 0);
    } else {
      // All four monochrome cases: black, white, transparent, INVERT (AND 1, XOR 1).
      const unsigned type = (x / 3 + y / 3) % 4;
      setBit(value.mask, width, y, x, type >= 2);
      setBit(value.mask, width, y + height, x, type == 1 || type == 3);
    }
  }
  return value;
}
OwnedCursor make(const Fixture& value) { return make(value.mask, value.colour, value.width, value.height, value.width - 1, value.height / 2); }
// The client's reconstruction (cursor_presenter / CreateRemoteCursorHandle):
// the same 1-bpp black/white mask DIB and 32-bpp colour DIB, CreateIconIndirect.
OwnedCursor recreate(const cursor::Shape& shape) { return make(shape.mask, shape.colour, shape.width, shape.height, shape.hotspotX, shape.hotspotY); }
// One fixture draw; the failing GDI step (0 = drawn). The DC is always released.
const char* drawOnce(HCURSOR handle, unsigned width, unsigned height, unsigned background, std::vector<uint8_t>& image) {
  constexpr unsigned side = 300;
  image.assign(size_t(side) * side * 4, 0);
  for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
    const size_t at = (size_t(y) * side + x) * 4; const uint8_t value = background == 0 ? 0 : background == 1 ? 255 : uint8_t(((x / 5 + y / 5) % 2) ? 185 : 47);
    image[at] = value; image[at + 1] = uint8_t(background == 2 ? value / 2 : value); image[at + 2] = value; image[at + 3] = 255;
  }
  struct Dc { HDC value; ~Dc() { if (value) DeleteDC(value); } } memory{CreateCompatibleDC(nullptr)};
  if (!memory.value) return "Draw fixture DC failed";
  Bitmap surface{dib(side, side, false, image)}; const auto previous = SelectObject(memory.value, surface.value);
  if (previous == nullptr || previous == HGDI_ERROR) return "Draw fixture select failed";
  const bool drawn = DrawIconEx(memory.value, 13, 17, handle, int(width), int(height), 0, nullptr, DI_NORMAL) != FALSE; GdiFlush();
  SelectObject(memory.value, previous);
  if (!drawn) return "DrawIconEx fixture failed";
  struct Info { BITMAPINFOHEADER header{}; } info; info.header.biSize = sizeof(BITMAPINFOHEADER); info.header.biWidth = LONG(side); info.header.biHeight = -LONG(side); info.header.biPlanes = 1; info.header.biBitCount = 32; info.header.biCompression = BI_RGB;
  if (GetDIBits(memory.value, surface.value, 0, side, image.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS) != int(side)) return "Draw fixture read failed";
  for (size_t at = 3; at < image.size(); at += 4) image[at] = 255;  // destination alpha is not shown
  return nullptr;
}
// A single GDI call can fail transiently on a loaded machine or in a
// disconnected/locked RDP session (a Host release stopped on "DrawIconEx
// fixture failed"; the case passed when rerun). Redraw from scratch a bounded
// number of times; the exact pixel comparison of the result is unchanged.
std::vector<uint8_t> draw(HCURSOR handle, unsigned width, unsigned height, unsigned background) {
  std::vector<uint8_t> image;
  for (int attempt = 1;; ++attempt) {
    const char* failure = drawOnce(handle, width, height, background, image);
    if (!failure) { require(true, "Draw fixture"); return image; }
    if (attempt >= 5) require(false, failure);
    Sleep(DWORD(20 * attempt));
  }
}
unsigned renderedCases = 0;
// Drawing the original and the cursor rebuilt from its record gives the same
// RGB on black, white and a checker: alpha, masks and inversion survive.
void requireRoundtrip(HCURSOR original, const cursor::Shape& shape) {
  const auto rebuilt = recreate(shape);
  ICONINFO info{}; require(GetIconInfo(reinterpret_cast<HICON>(rebuilt.value), &info) != FALSE, "Rebuilt cursor unreadable");
  if (info.hbmMask) DeleteObject(info.hbmMask); if (info.hbmColor) DeleteObject(info.hbmColor);
  require(!info.fIcon && info.xHotspot == shape.hotspotX && info.yHotspot == shape.hotspotY, "Rebuilt cursor lost its hotspot");
  for (unsigned background = 0; background < 3; ++background) {
    require(draw(original, shape.width, shape.height, background) == draw(rebuilt.value, shape.width, shape.height, background), "Cursor record does not redraw like the original");
    ++renderedCases;
  }
}

void conversionPerKind() {
  for (const auto& [kind, width, height] : std::vector<std::tuple<Kind, unsigned, unsigned>>{{Kind::alpha, 37, 29}, {Kind::masked, 33, 21}, {Kind::mono, 31, 17}, {Kind::mono, 33, 19}, {Kind::alpha, 256, 256}, {Kind::mono, 256, 256}}) {
    const auto value = fixture(kind, width, height); const auto handle = make(value);
    const auto shape = cursor::extract(handle.value);
    require(shape.has_value(), "A representable cursor was not extracted");
    require(shape->format == (kind == Kind::mono ? cursor::Format::monochrome : cursor::Format::colour) && shape->width == width && shape->height == height, "Cursor format or size changed");
    require(shape->hotspotX == width - 1 && shape->hotspotY == height / 2, "Cursor hotspot changed");
    require(cursor::validShape(*shape) && shape->mask.size() == value.mask.size(), "Extracted shape is not canonical");
    for (unsigned y = 0; y < height * (kind == Kind::mono ? 2 : 1); ++y) for (unsigned x = 0; x < width; ++x)
      if (bit(shape->mask, width, y, x) != bit(value.mask, width, y, x)) require(false, "AND/XOR mask bits changed");
    if (kind != Kind::mono) require(shape->colour == value.colour, "Colour/alpha bytes changed");
    if (kind == Kind::alpha) require(shape->colour[3] == 0 && shape->colour[3 + 4 * 3] == 85 && shape->colour[3 + 4 * 9] == 255, "Alpha was lost");
    requireRoundtrip(handle.value, *shape);
  }
  // The shared system cursors (scheme dependent: colour-alpha arrow, inverting
  // or colour I-beam, animated busy cursors whose first frame is used).
  unsigned systemCursors = 0;
  for (const auto id : {IDC_ARROW, IDC_IBEAM, IDC_WAIT, IDC_APPSTARTING, IDC_HAND, IDC_SIZEALL, IDC_CROSS, IDC_NO}) {
    const auto handle = LoadCursorW(nullptr, id); require(handle != nullptr, "System cursor unavailable");
    const auto shape = cursor::extract(handle); require(shape.has_value(), "A system cursor could not be represented");
    requireRoundtrip(handle, *shape); ++systemCursors;
  }
  require(systemCursors == 8, "System cursor coverage");
  // Not representable: the capture composes these instead of sending them.
  require(!cursor::extract(nullptr), "A null cursor was represented");
  { const auto big = make(fixture(Kind::alpha, 257, 8)); require(!cursor::extract(big.value), "A cursor beyond 256 pixels escaped the bound"); }
  { const auto big = make(fixture(Kind::mono, 8, 257)); require(!cursor::extract(big.value), "A tall monochrome cursor escaped the bound"); }
}
// Extraction copies the handle and reads its bitmaps: nothing may accumulate.
void extractionLifetime() {
  const auto value = fixture(Kind::alpha, 48, 48); const auto handle = make(value);
  for (int n = 0; n < 3; ++n) { const auto shape = cursor::extract(handle.value); require(shape.has_value(), "Warm-up extraction failed"); const auto rebuilt = recreate(*shape); }
  const auto gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  for (int n = 0; n < 300; ++n) {
    const auto shape = cursor::extract(n % 2 ? handle.value : LoadCursorW(nullptr, IDC_IBEAM)); require(shape.has_value(), "Extraction failed");
    const auto record = cursor::shapeRecord(*shape, uint32_t(n + 1)); const auto rebuilt = recreate(*shape); require(!record.empty(), "Record");
  }
  require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdi && GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == user, "Cursor extraction leaked GDI/USER handles");
}

void records() {
  const auto value = fixture(Kind::mono, 33, 19); const auto handle = make(value);
  const auto shape = *cursor::extract(handle.value);
  const auto record = cursor::shapeRecord(shape, 0x01020304);
  require(record.size() == 16 + cursor::maskStride(33) * 38, "Monochrome shape record length");
  require(record[0] == 1 && record[1] == 2 && record[2] == 0 && record[3] == 0 && record[4] == 4 && record[7] == 1 && record[8] == 33 && record[10] == 19 && record[12] == 32 && record[14] == 9, "Shape record header layout");
  require(std::equal(shape.mask.begin(), shape.mask.end(), record.begin() + 16), "Shape record mask bytes");
  const auto colour = *cursor::extract(make(fixture(Kind::alpha, 37, 29)).value);
  require(cursor::shapeRecord(colour, 1).size() == 16 + cursor::maskStride(37) * 29 + 37 * 29 * 4, "Colour shape record length");
  const auto hidden = cursor::stateRecord(false, 0, std::nullopt);
  require(hidden == std::vector<uint8_t>{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "Hidden state layout");
  const auto warped = cursor::stateRecord(true, 7, POINT{0x0102, 0x0304});
  require(warped == std::vector<uint8_t>{2, 3, 0, 0, 7, 0, 0, 0, 2, 1, 0, 0, 4, 3, 0, 0}, "Visible state with position layout");
  bool rejected = 0;
  for (const auto& bad : std::vector<std::function<void()>>{[] { cursor::stateRecord(true, 0, std::nullopt); }, [] { cursor::stateRecord(false, 3, std::nullopt); },
       [] { cursor::stateRecord(true, 1, POINT{-1, 0}); }, [&] { cursor::shapeRecord(shape, 0); }, [&] { cursor::shapeRecord(shape, 0x80000000u); }}) {
    try { bad(); rejected = false; } catch (const std::exception&) { rejected = true; }
    require(rejected, "An invalid record was serialized");
  }
  // The media envelope: kind 3, flags 0, the epoch stamped like video.
  auto packet = mediaPacket(cursor::mediaKind, 1234, hidden);
  stampMediaEpoch(packet, 0x0201);
  require(packet.size() == 28 && packet[0] == 3 && packet[1] == 0 && packet[2] == 1 && packet[3] == 2 && packet[4] == 0xd2 && packet[5] == 0x04, "Cursor media envelope or epoch stamp");
}

// Channel scenarios with substituted extraction (fake handles, no GDI).
struct Fake {
  std::map<uintptr_t, std::optional<cursor::Shape>> table; unsigned extractions = 0;
  cursor::Channel::Extract extractor() { return [this](HCURSOR handle) { ++extractions; const auto found = table.find(reinterpret_cast<uintptr_t>(handle)); return found == table.end() ? std::nullopt : found->second; }; }
};
cursor::Shape synthetic(uint8_t seed, uint16_t side = 8) {
  cursor::Shape shape; shape.format = cursor::Format::monochrome; shape.width = side; shape.height = side; shape.hotspotX = 1; shape.hotspotY = 2;
  shape.mask.assign(cursor::maskStride(side) * side * 2, 0); shape.mask[0] = uint8_t(seed & 0xc0) ? uint8_t(seed & 0xc0) : 0x80; shape.mask[size_t(side) * cursor::maskStride(side)] = uint8_t(seed << 2 & 0xff) & 0xff;
  for (size_t row = 0; row < size_t(side) * 2; ++row) shape.mask[row * cursor::maskStride(side) + 0] = uint8_t((row * 37 + seed) & 0xff);
  return shape;
}
HCURSOR fakeHandle(uintptr_t value) { return reinterpret_cast<HCURSOR>(value); }
struct Decoded { uint8_t type = 0; uint8_t flags = 0; uint32_t id = 0; int32_t x = 0, y = 0; };
Decoded decode(const std::vector<uint8_t>& record) {
  Decoded value; value.type = record[0]; value.flags = record[1];
  value.id = uint32_t(record[4]) | uint32_t(record[5]) << 8 | uint32_t(record[6]) << 16 | uint32_t(record[7]) << 24;
  if (value.type == 2) { value.x = int32_t(uint32_t(record[8]) | uint32_t(record[9]) << 8 | uint32_t(record[10]) << 16 | uint32_t(record[11]) << 24); value.y = int32_t(uint32_t(record[12]) | uint32_t(record[13]) << 8 | uint32_t(record[14]) << 16 | uint32_t(record[15]) << 24); }
  return value;
}
cursor::Observation at(HCURSOR handle, LONG x, LONG y, DWORD flags = CURSOR_SHOWING) { return {true, flags, handle, POINT{x, y}}; }

void channelShapesAndVisibility() {
  Fake fake; for (uintptr_t n = 1; n <= 40; ++n) fake.table[0x100 + n] = synthetic(uint8_t(n));
  fake.table[0x900] = synthetic(1);   // same content as 0x101 under another handle
  fake.table[0x666] = std::nullopt;   // unrepresentable
  cursor::SessionShapeIds ids; const RECT source{100, 50, 100 + 1920, 50 + 1080};
  cursor::Channel channel(source, [&] { return ids.next("s1"); }, fake.extractor());
  uint64_t now = 1000000; cursor::InjectedPointer::Mark none;
  // Ticks 30 ms apart: beyond the 25 ms state coalescing (tested separately).
  auto tick = [&](const cursor::Observation& seen) { now += 30000; return channel.observe(seen, none, now); };
  require(tick(at(fakeHandle(0x101), 500, 400)).empty(), "Records were sent before the first video key frame");
  channel.keyFrame();
  auto out = tick(at(fakeHandle(0x101), 500, 400));
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).type == 2 && decode(out[1]).flags == 1 && decode(out[1]).id == decode(out[0]).id, "The first key frame did not bring the shape followed by its visible state");
  const auto first = decode(out[0]).id;
  require(tick(at(fakeHandle(0x101), 520, 420)).empty() && tick(at(fakeHandle(0x101), 540, 440)).empty(), "A cursor-only move sent records");
  const auto extracted = fake.extractions;
  for (int n = 0; n < 20; ++n) tick(at(fakeHandle(0x101), 540 + n, 440));
  require(fake.extractions == extracted, "An unchanged handle was re-read every tick");
  out = tick(at(fakeHandle(0x102), 540, 440));
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[0]).id > first && decode(out[1]).id == decode(out[0]).id, "A new shape did not get a new increasing id");
  out = tick(at(fakeHandle(0x101), 540, 440));
  require(out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).id == first, "A cached shape was sent again instead of naming its id");
  out = tick(at(fakeHandle(0x900), 540, 440));
  require(out.empty(), "Identical content under another handle was not recognised");
  // Hidden: not showing, suppressed (touch/pen), null handle, outside the output.
  for (const auto& hidden : {at(fakeHandle(0x101), 540, 440, 0), at(fakeHandle(0x101), 540, 440, CURSOR_SHOWING | CURSOR_SUPPRESSED), at(nullptr, 540, 440), at(fakeHandle(0x101), 99, 440), at(fakeHandle(0x101), 2020, 440)}) {
    now += 100000;
    out = channel.observe(hidden, none, now);
    require(out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).flags == 0 && decode(out[0]).id == 0, "A hidden cursor was not reported hidden");
    now += 100000;
    out = channel.observe(at(fakeHandle(0x101), 540, 440), none, now);
    require(out.size() == 1 && decode(out[0]).flags == 1 && decode(out[0]).id == first, "Visibility did not return");
  }
  // An unknown observation is not evidence Windows hid the cursor. The
  // production contract preserves the last visibility/composition choice.
  now += 100000;
  require(channel.observe(cursor::Observation{}, none, now).empty() && !channel.composeFallback(), "An unknown observation changed a known visible cursor");
  now += 100000;
  require(channel.observe(at(fakeHandle(0x101), 540, 440), none, now).empty(), "A known cursor was unnecessarily re-announced after an unknown observation");
  require(!channel.composeFallback(), "A representable cursor was composed");
  // Unrepresentable: composed into the video, reported hidden, read once.
  now += 100000; out = channel.observe(at(fakeHandle(0x666), 540, 440), none, now);
  require(channel.composeFallback() && out.size() == 1 && decode(out[0]).flags == 0, "An unrepresentable cursor was not composed and hidden");
  const auto reads = fake.extractions;
  require(tick(at(fakeHandle(0x666), 560, 440)).empty() && channel.composeFallback() && fake.extractions == reads, "Fallback did not persist without re-reading");
  require(tick(at(fakeHandle(0x666), 50, 20)).empty() && !channel.composeFallback(), "An unrepresentable cursor outside the output was composed");
  now += 100000; out = channel.observe(at(fakeHandle(0x101), 540, 440), none, now);
  require(!channel.composeFallback() && out.size() == 1 && decode(out[0]).id == first, "Fallback did not end with a representable cursor");
  // In-place scheme change: the same handle is re-read once per second.
  fake.table[0x101] = synthetic(77);
  require(tick(at(fakeHandle(0x101), 540, 440)).empty(), "Content was re-read before its period");
  now += cursor::revalidateUs; out = channel.observe(at(fakeHandle(0x101), 540, 440), none, now);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[0]).id > first, "An in-place shape change kept the old id");
  const auto counted = channel.takeCounters();
  require(counted.shapes >= 3 && counted.states >= 10 && counted.composed >= 2 && channel.takeCounters().shapes == 0, "Channel counters");
}

// The SYSTEM helper's frame of a secure desktop (secure_desktop_switch.hpp) has that desktop's pointer composed
// in: the client is told hidden once, nothing else changes, and the capture's next tick reports the real cursor.
void channelHideForSecureDesktop() {
  Fake fake; fake.table[0x101] = synthetic(1);
  cursor::SessionShapeIds ids; const RECT source{0, 0, 1920, 1080};
  cursor::Channel channel(source, [&] { return ids.next("s9"); }, fake.extractor());
  uint64_t now = 1000000; cursor::InjectedPointer::Mark none;
  require(channel.hide(now).empty(), "A hidden state was sent before the first video key frame");
  channel.keyFrame();
  now += 100000; auto out = channel.observe(at(fakeHandle(0x101), 500, 400), none, now);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).flags == 1, "The visible cursor before the secure desktop was not sent");
  const auto id = decode(out[0]).id;
  now += 100000; out = channel.hide(now);
  require(out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).flags == 0 && decode(out[0]).id == 0, "The helper's frame did not hide the client's pointer");
  for (int n = 0; n < 50; ++n) { now += 16000; require(channel.hide(now).empty(), "Hidden was repeated while the helper's frame showed"); }
  require(!channel.composeFallback(), "Hiding for the helper's frame changed the capture's composition");
  now += 100000; out = channel.observe(at(fakeHandle(0x101), 500, 400), none, now);
  require(out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).flags == 1 && decode(out[0]).id == id, "The real cursor did not return after the secure desktop");
  now += 100000;
  require(channel.observe(at(fakeHandle(0x101), 500, 400), none, now).empty(), "A cursor that did not move was announced again after the secure desktop");
}

void channelCacheBoundsAndRates() {
  Fake fake; for (uintptr_t n = 1; n <= 80; ++n) fake.table[0x100 + n] = synthetic(uint8_t(n));
  cursor::SessionShapeIds ids; const RECT source{0, 0, 1920, 1080};
  cursor::Channel channel(source, [&] { return ids.next("s2"); }, fake.extractor());
  channel.keyFrame(); cursor::InjectedPointer::Mark none;
  // 15 new shapes in 150 ms: at most 10 shape records per second; no state
  // ever names an id whose shape was not sent first.
  uint64_t now = 5000000; std::set<uint32_t> sent; unsigned shapes = 0;
  const auto check = [&](const std::vector<std::vector<uint8_t>>& out) {
    for (const auto& record : out) {
      const auto value = decode(record);
      if (value.type == 1) { require(sent.insert(value.id).second && (sent.empty() || value.id == *sent.rbegin()), "Shape ids did not increase"); ++shapes; }
      else if (value.flags & 1) require(sent.contains(value.id), "A state named an unsent shape");
    }
  };
  for (uintptr_t n = 1; n <= 15; ++n) { now += 10000; check(channel.observe(at(fakeHandle(0x100 + n), 10, 10), none, now)); }
  require(shapes == 10, "More than 10 shape records in one second");
  for (int n = 0; n < 100; ++n) { now += 10000; check(channel.observe(at(fakeHandle(0x10f), 10, 10), none, now)); }
  require(shapes == 11, "A waiting shape was not sent once its budget returned");
  // States coalesce: a visibility flip every 5 ms yields at most one per 25 ms.
  unsigned states = 0; const auto begin = now;
  for (int n = 0; n < 200; ++n) { now += 5000; for (const auto& record : channel.observe(at(fakeHandle(0x10f), 10, 10, n % 2 ? CURSOR_SHOWING : 0), none, now)) if (record[0] == 2) ++states; }
  require(states <= (now - begin) / cursor::stateIntervalUs + 1 && states >= 10, "States were not coalesced to the 25 ms bound");
  // LRU: at most 32 live shapes; an evicted shape returns under a new id.
  for (uintptr_t n = 20; n <= 60; ++n) { now += 200000; check(channel.observe(at(fakeHandle(0x100 + n), 10, 10), none, now)); }
  require(channel.liveShapes() <= cursor::liveShapeLimit, "The shape cache exceeded its bound");
  const auto before = *sent.rbegin();
  now += 200000; auto out = channel.observe(at(fakeHandle(0x114), 10, 10), none, now);
  require(!out.empty() && decode(out[0]).type == 1 && decode(out[0]).id > before, "An evicted shape was named instead of re-sent");
  // Ids keep increasing across epochs of one session; a new session restarts.
  cursor::Channel next(source, [&] { return ids.next("s2"); }, fake.extractor()); next.keyFrame();
  out = next.observe(at(fakeHandle(0x101), 10, 10), none, now + 1000);
  require(out.size() == 2 && decode(out[0]).id > *sent.rbegin(), "A resize restart reused a shape id");
  require(ids.next("s3") == 1, "A new session did not restart its ids");
}

// stream-size-v1: the encoder restarts inside the epoch and the client's new
// decoder has forgotten every shape and state (cursor_channel.hpp
// pictureRestart). Nothing leaves before the new key frame; then the current
// shape is sent again under a new id with its state although the cursor did
// not change, and no state names an id from before the restart.
void channelPictureRestart() {
  Fake fake; for (uintptr_t n = 1; n <= 3; ++n) fake.table[0x100 + n] = synthetic(uint8_t(n));
  cursor::SessionShapeIds ids; const RECT source{0, 0, 1920, 1080};
  cursor::Channel channel(source, [&] { return ids.next("restart"); }, fake.extractor());
  uint64_t now = 1000000; cursor::InjectedPointer::Mark none;
  auto tick = [&](uintptr_t handle) { now += 30000; return channel.observe(at(fakeHandle(handle), 500, 400), none, now); };
  channel.keyFrame();
  auto out = tick(0x101); require(out.size() == 2, "The first shape and state");
  const auto arrow = decode(out[0]).id;
  out = tick(0x102); require(out.size() == 2, "The second shape and state");
  const auto edge = decode(out[0]).id;
  require(tick(0x101).size() == 1 && channel.liveShapes() == 2, "A cached shape before the restart");
  channel.pictureRestart();
  require(!channel.ready() && channel.liveShapes() == 0, "A restart kept shapes the client forgot");
  require(tick(0x101).empty() && tick(0x102).empty() && tick(0x101).empty(), "Records left before the restarted picture's key frame");
  require(!channel.echo(at(fakeHandle(0x101), 500, 400), now + 1000), "A position was echoed before the restarted picture's state");
  channel.keyFrame();
  out = tick(0x101);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[0]).id > edge && decode(out[1]).type == 2 && decode(out[1]).flags == 1 && decode(out[1]).id == decode(out[0]).id,
          "The unchanged cursor was not sent again to the restarted decoder");
  const auto arrowAgain = decode(out[0]).id;
  out = tick(0x102);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[0]).id > arrowAgain && decode(out[1]).id == decode(out[0]).id && decode(out[1]).id != edge,
          "A state named a shape id from before the restart");
  out = tick(0x101);
  require(out.size() == 1 && decode(out[0]).id == arrowAgain && arrowAgain != arrow, "A shape sent after the restart was not named by its new id");
  // A hidden cursor is announced to the restarted decoder too.
  channel.pictureRestart(); channel.keyFrame();
  now += 30000; out = channel.observe(at(fakeHandle(0x101), 500, 400, 0), none, now);
  require(out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).flags == 0, "Hidden was not announced to the restarted decoder");
  // Through the shared channel as the capture thread uses it.
  cursor::SharedChannel shared(source, cursor::Channel::Allocate([&] { return ids.next("restart"); }), fake.extractor());
  std::vector<Decoded> sent; const auto emit = [&](const std::vector<uint8_t>& record) { sent.push_back(decode(record)); };
  shared.keyFrame(); shared.observe(at(fakeHandle(0x103), 10, 10), none, now += 30000, emit);
  require(sent.size() == 2, "Shared channel: the first shape and state");
  shared.pictureRestart(); sent.clear();
  shared.observe(at(fakeHandle(0x103), 10, 10), none, now += 30000, emit);
  require(sent.empty(), "Shared channel: records left before the restarted picture's key frame");
  shared.keyFrame(); shared.observe(at(fakeHandle(0x103), 10, 10), none, now += 30000, emit);
  require(sent.size() == 2 && sent[0].type == 1 && sent[1].type == 2 && sent[1].id == sent[0].id, "Shared channel: the cursor was not sent again");
}

// Real Channel, injected extraction/clock only. The broker's cursorPacket
// contract requires strictly increasing SHAPE ids; cached STATE ids may be
// older, but must name a shape already published in this session.
void channelPendingShapesPublishInOrder() {
  Fake fake; for (uintptr_t n = 1; n <= 12; ++n) fake.table[0x100 + n] = synthetic(uint8_t(n));
  cursor::SessionShapeIds ids; const RECT source{0, 0, 1920, 1080};
  cursor::Channel channel(source, [&] { return ids.next("pending-order"); }, fake.extractor());
  channel.keyFrame(); cursor::InjectedPointer::Mark none;
  uint64_t now = 1000000; uint32_t lastShape = 0; std::set<uint32_t> published;
  const auto accept = [&](const std::vector<std::vector<uint8_t>>& out) {
    for (const auto& record : out) {
      const auto value = decode(record);
      if (value.type == cursor::shapeRecordType) {
        if (value.id <= lastShape) throw std::runtime_error("Pending cursor shape violated broker strict send order: previous=" + std::to_string(lastShape) + ", current=" + std::to_string(value.id));
        require(published.insert(value.id).second, "A shape was published twice");
        lastShape = value.id;
      } else if (value.flags & 1) require(published.contains(value.id), "A state named a budget-deferred shape before publication");
    }
  };
  const auto tick = [&](uintptr_t handle) { return channel.observe(at(fakeHandle(handle), 10, 10), none, now); };
  // Fill the actual ten-shapes/s budget, without hitting the state coalescer.
  for (uintptr_t n = 1; n <= cursor::shapesPerSecond; ++n) { now += 30000; accept(tick(0x100 + n)); }
  require(published.size() == cursor::shapesPerSecond, "The initial shape budget was not filled");
  // A and B are both resolved while the send budget is exhausted. The old
  // producer reserves A=11/B=12 here, then can publish them in reverse order.
  now += 30000; auto out = tick(0x10b); accept(out);
  require(out.empty(), "Deferred A exceeded the shape/state budget");
  now += 30000; out = tick(0x10c); accept(out);
  require(out.empty(), "Deferred B exceeded the shape/state budget");
  require(channel.liveShapes() == 12, "The deferred shapes did not remain in the bounded cache");
  // The next chosen shape is B, then the older deferred A is revisited.
  // This must be acceptable to the existing strict broker, without relaxing it.
  now = 2500000; out = tick(0x10c); accept(out);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).id == decode(out[0]).id, "B did not publish before its visible state once budget returned");
  const uint32_t b = decode(out[0]).id;
  now += 30000; out = tick(0x10b); accept(out);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).id == decode(out[0]).id && decode(out[0]).id > b, "A did not receive a publishable id after B");
  // Already-published B remains a cache hit: its old id is legal in STATE,
  // but no duplicate or decreasing SHAPE record may be emitted.
  now += 30000; out = tick(0x10c); accept(out);
  require(out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).id == b, "Published B was not reused as a state-only cache hit");
  require(published.size() == 12 && channel.liveShapes() <= cursor::liveShapeLimit, "Deferred ordering changed the publication/cache bounds");
}

void channelPendingShapeIdExhaustion() {
  Fake fake; for (uintptr_t n = 1; n <= 12; ++n) fake.table[0x100 + n] = synthetic(uint8_t(n));
  uint32_t allocated = 0;
  cursor::Channel channel(RECT{0, 0, 1920, 1080}, [&] { return allocated < 12 ? ++allocated : 0; }, fake.extractor());
  channel.keyFrame(); cursor::InjectedPointer::Mark none; uint64_t now = 1000000;
  for (uintptr_t n = 1; n <= 12; ++n) { now += 30000; channel.observe(at(fakeHandle(0x100 + n), 10, 10), none, now); }
  now = 2500000; const auto b = channel.observe(at(fakeHandle(0x10c), 10, 10), none, now);
  require(b.size() == 2 && decode(b[0]).type == 1 && decode(b[0]).id == 12, "Exhaustion fixture did not first publish newer deferred B");
  now += 30000; const auto a = channel.observe(at(fakeHandle(0x10b), 10, 10), none, now);
  require(a.size() == 1 && decode(a[0]).type == 2 && decode(a[0]).flags == 0 && decode(a[0]).id == 0 && channel.composeFallback(), "Exhausted deferred id was sent instead of hidden/composed fallback");
  now += 30000;
  require(channel.observe(cursor::Observation{}, none, now).empty() && channel.composeFallback(), "An unknown observation cleared the exhausted-id composition fallback");
  now += 30000; const auto cached = channel.observe(at(fakeHandle(0x10c), 10, 10), none, now);
  require(cached.size() == 1 && decode(cached[0]).type == 2 && decode(cached[0]).id == 12 && !channel.composeFallback(), "Id exhaustion discarded an already-published cache entry");
}

void channelProtocolRecords() {
  const auto shape = synthetic(17);
  const auto wire = cursor::shapeRecord(shape, 7);
  require(wire.size() == 16 + shape.mask.size() && decode(wire).type == 1 && decode(wire).id == 7, "Injected monochrome shape wire changed");
  const auto state = cursor::stateRecord(true, 7, POINT{10, 20});
  require(state.size() == 16 && decode(state).flags == 3 && decode(state).id == 7 && decode(state).x == 10 && decode(state).y == 20, "Injected visible state wire changed");
  auto packet = mediaPacket(cursor::mediaKind, 1234, state); stampMediaEpoch(packet, 0x0201);
  require(packet.size() == 28 && packet[0] == 3 && packet[1] == 0 && packet[2] == 1 && packet[3] == 2, "Injected cursor media envelope changed");
  for (const auto& bad : std::vector<std::function<void()>>{[] { cursor::stateRecord(true, 0, std::nullopt); }, [] { cursor::stateRecord(false, 3, std::nullopt); },
       [] { cursor::stateRecord(true, 1, POINT{-1, 0}); }, [&] { cursor::shapeRecord(shape, 0); }}) {
    bool rejected = false; try { bad(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Injected invalid cursor record was serialized");
  }
}

void channelWarps() {
  Fake fake; fake.table[0x101] = synthetic(1);
  cursor::SessionShapeIds ids; const RECT source{-1920, 0, 0, 1080};
  cursor::Channel channel(source, [&] { return ids.next("s4"); }, fake.extractor()); channel.keyFrame();
  cursor::InjectedPointer input; uint64_t now = 9000000;
  auto tick = [&](LONG x, LONG y, uint64_t step = 16667) { now += step; return channel.observe(at(fakeHandle(0x101), x, y), input.read(), now); };
  const auto warps = [](const std::vector<std::vector<uint8_t>>& out) { std::vector<Decoded> found; for (const auto& r : out) if (r[0] == 2 && (r[1] & 2)) found.push_back(decode(r)); return found; };
  tick(-1000, 500); tick(-1000, 500);
  require(warps(tick(-1000, 500)).empty(), "The baseline position was reported");
  // Our own injected input: the cursor follows it within rounding.
  for (int n = 0; n < 20; ++n) { input.injected(POINT{-1000 + n * 5, 500}, now); require(warps(tick(-1000 + n * 5 + (n % 2), 500)).empty(), "Injected movement was reported as a warp"); }
  // An application warp while the user still moves: never reported mid-movement.
  input.injected(POINT{-900, 500}, now);
  require(warps(tick(-500, 300, 1000)).empty() && warps(tick(-500, 300, 1000)).empty(), "A warp was reported within the quiet period after injection");
  // Once quiet and stable: one position, relative to the output.
  std::vector<Decoded> found; for (int n = 0; n < 10 && found.empty(); ++n) found = warps(tick(-500, 300));
  require(found.size() == 1 && found[0].x == 1420 && found[0].y == 300 && (found[0].flags & 1), "An application warp was not reported at the source position");
  for (int n = 0; n < 20; ++n) require(warps(tick(-500, 300)).empty(), "A reported warp repeated");
  // Unstable (moving without our input) is never reported; a second warp waits 100 ms.
  for (int n = 0; n < 20; ++n) require(warps(tick(-400 + n * 7, 300, 4000)).empty(), "A continuously moving cursor was warped");
  const auto settle = now; found.clear();
  for (int n = 0; n < 20 && found.empty(); ++n) found = warps(tick(-200, 200, 4000));
  require(found.size() == 1 && now - settle >= 8000, "A settled foreign move was not reported");
  found.clear(); for (int n = 0; n < 5; ++n) { const auto w = warps(tick(-150, 150, 4000)); found.insert(found.end(), w.begin(), w.end()); }
  require(found.empty(), "Two warps within 100 ms");
  for (int n = 0; n < 40 && found.empty(); ++n) found = warps(tick(-150, 150, 4000));
  require(found.size() == 1 && found[0].x == 1770 && found[0].y == 150, "The next warp did not follow after its interval");
  // A position outside the owned output is never sent.
  for (int n = 0; n < 20; ++n) require(warps(tick(100, 100)).empty(), "A position outside the output was sent");
  require(channel.takeCounters().warps == 3, "Warp counter");
}

// cursor-tracks-v1: one channel over the owned outputs of independent outputs
// (the track source's unequal-height fixture). Visible on either output, never
// a position, the unrepresentable fallback on either output.
void channelOutputs() {
  Fake fake; fake.table[0x101] = synthetic(1); fake.table[0x666] = std::nullopt;
  cursor::SessionShapeIds ids; const std::vector<RECT> outputs{{-1600, 300, -960, 660}, {-960, 400, -160, 1000}};
  cursor::Channel channel(outputs, [&] { return ids.next("outputs"); }, fake.extractor()); channel.keyFrame();
  cursor::InjectedPointer::Mark none; uint64_t now = 1000000;
  auto tick = [&](LONG x, LONG y, uint64_t step = 30000) { now += step; return channel.observe(at(fakeHandle(0x101), x, y), none, now); };
  auto out = tick(-1300, 400);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).flags == 1 && decode(out[1]).id == decode(out[0]).id, "The first output did not bring the shape and its visible state");
  const auto id = decode(out[0]).id;
  require(tick(-500, 900).empty() && tick(-1000, 650).empty(), "Moving between the owned outputs sent records");
  // Below the shorter first output: inside neither output.
  out = tick(-1300, 700, 100000);
  require(out.size() == 1 && decode(out[0]).flags == 0 && decode(out[0]).id == 0, "A hotspot outside every owned output was not hidden");
  out = tick(-200, 999, 100000);
  require(out.size() == 1 && decode(out[0]).flags == 1 && decode(out[0]).id == id, "Visibility did not return on the second output");
  // Settled moves without our input (a single output reports these as warps).
  for (int n = 0; n < 40; ++n)
    for (const auto& record : tick((n / 5) % 2 ? -1500 : -300, 450, 120000)) require(!(record[0] == 2 && (record[1] & 2)), "Independent outputs sent a cursor position");
  require(channel.takeCounters().warps == 0, "Independent outputs counted a warp");
  now += 100000; out = channel.observe(at(fakeHandle(0x666), -500, 900), none, now);
  require(channel.composeFallback() && out.size() == 1 && decode(out[0]).flags == 0, "An unrepresentable cursor on the second output was not composed and hidden");
  for (const auto& bad : {std::vector<RECT>{}, std::vector<RECT>{{0, 0, 16, 16}, {16, 0, 32, 16}, {32, 0, 48, 16}}}) {
    bool rejected = false; try { cursor::Channel invalid(bad, [] { return 1u; }, fake.extractor()); } catch (const std::exception&) { rejected = true; }
    require(rejected, "A cursor channel accepted no or three outputs");
  }
}

// cursor-echo-v1: the session marker and the client's request.
void echoGatingAndRequests() {
  const auto parse = [](const std::string& extra, const char* mode = "stream") { return JsonParser::parse(std::string("{\"type\":\"session.start\",\"mode\":\"") + mode + "\",\"sessionId\":\"x\"" + extra + '}'); };
  const std::string both = ",\"cursorProtocol\":\"cursor-v1\",\"cursorEchoProtocol\":\"cursor-echo-v1\"";
  require(std::string(cursor::echoProtocol) == "cursor-echo-v1", "Echo protocol marker changed");
  require(cursor::echoSessionRequested(parse(both)), "A granted echo session was not recognized");
  require(!cursor::echoSessionRequested(parse(",\"cursorEchoProtocol\":\"cursor-echo-v1\"")), "Echo without cursor-v1 was granted");
  require(!cursor::echoSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\"")), "cursor-v1 alone granted echo");
  require(!cursor::echoSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\",\"cursorEchoProtocol\":\"cursor-echo-v2\"")), "Another echo value was granted");
  require(!cursor::echoSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\",\"cursorEchoProtocol\":true")), "A non-string echo marker was granted");
  require(!cursor::echoSessionRequested(parse(both + ",\"mediaProtocol\":\"tracks-v1\"")), "Independent outputs were granted echo");
  require(!cursor::echoSessionRequested(parse(both, "rdp")), "RDP was granted echo");
  require(cursor::echoRequest(JsonParser::parse(R"({"type":"cursor.echo","on":true})")) == std::optional<bool>(true), "echo on refused");
  require(cursor::echoRequest(JsonParser::parse(R"({"type":"cursor.echo","on":false,"sessionId":"x"})")) == std::optional<bool>(false), "echo off with sessionId refused");
  for (const char* bad : {R"({"type":"cursor.echo"})", R"({"type":"cursor.echo","on":1})", R"({"type":"cursor.echo","on":"true"})", R"({"type":"cursor.echo","on":true,"rate":2})", R"({"type":"caret.watch","on":true})", R"([true])"})
    require(!cursor::echoRequest(JsonParser::parse(bad)), "An invalid echo request was accepted");
}

// cursor-echo-v1 on the channel: positions at once while asked, the visible
// state and shape the client already has, never before the first state, never
// outside the output or on independent outputs, one per position, at most one
// per echoMinimumUs; the regular path neither repeats them as warps nor loses
// its own budget to them; the unrepresentable fallback is unchanged.
void channelEcho() {
  Fake fake; fake.table[0x101] = synthetic(1); fake.table[0x102] = synthetic(2); fake.table[0x666] = std::nullopt;
  cursor::SessionShapeIds ids; const RECT source{-1920, 0, 0, 1080};
  cursor::Channel channel(source, [&] { return ids.next("echo"); }, fake.extractor());
  cursor::InjectedPointer::Mark none; uint64_t now = 2000000;
  const auto poll = [&](LONG x, LONG y, uint64_t step = cursor::echoPollUs) { now += step; return channel.echo(at(fakeHandle(0x101), x, y), now); };
  require(!poll(-1000, 500), "An echo left before the first key frame");
  channel.keyFrame();
  require(!poll(-1000, 500), "An echo left before the first state");
  now += 30000; auto out = channel.observe(at(fakeHandle(0x101), -1000, 500), none, now);
  require(out.size() == 2, "The first state did not follow the key frame");
  const auto id = decode(out[0]).id;
  auto echoed = poll(-1000, 500);
  require(echoed && decode(*echoed).type == 2 && decode(*echoed).flags == 3 && decode(*echoed).id == id && decode(*echoed).x == 920 && decode(*echoed).y == 500, "The first echo is not the visible state with the output position");
  require(!poll(-1000, 500), "An unchanged position was echoed again");
  // Continuous movement: a new position at every poll that keeps the spacing (no stillness, no quiet period, no 10/s bound).
  unsigned moving = 0;
  for (int n = 1; n <= 200; ++n) if (const auto r = poll(-1000 + n, 500 + n / 2, cursor::echoMinimumUs)) { ++moving; require(decode(*r).x == 920 + n && decode(*r).y == 500 + n / 2, "An echo carried another position"); }
  require(moving == 200, "Moving positions were not all echoed");
  // Polled every 2 ms while it moves: at most 240 a second (each one the
  // poll's newest position), and the resting position still leaves after the spacing.
  require(cursor::echoMinimumUs * 240 >= 1000000, "Echo spacing allows more than 240 a second");
  unsigned perSecond = 0;
  for (int n = 1; n <= 500; ++n) if (const auto r = poll(-800 + n % 300, 400)) { ++perSecond; require(decode(*r).x == 1120 + n % 300, "A rate-bound echo was not the newest position"); }
  require(perSecond <= 240 && perSecond >= 160, "Echoes per second of 2 ms polls");
  std::optional<std::vector<uint8_t>> rest; for (int n = 0; n < 5 && !rest; ++n) rest = poll(-650, 410);
  require(rest && decode(*rest).x == 1270 && decode(*rest).y == 410, "The resting position did not leave after the spacing");
  // Never two within echoMinimumUs: the newest position follows at the next poll.
  require(!poll(-700, 600, cursor::echoMinimumUs - 1), "Two echoes within the minimum spacing");
  const auto later = poll(-700, 600, 1);
  require(later && decode(*later).x == 1220, "The newest position did not follow the minimum spacing");
  // Outside the output: nothing; back inside: sent.
  require(!poll(100, 100) && !poll(-1921, 10), "A position outside the output was echoed");
  require(poll(-1919, 10).has_value(), "A position back inside was not echoed");
  // The regular path does not report an echoed position again as a warp, and
  // keeps its own budget: a shape change during echoing still leaves at once.
  for (int n = 0; n < 10; ++n) { now += 16667; for (const auto& r : channel.observe(at(fakeHandle(0x101), -1919, 10), none, now)) require(!(r[1] & 2), "An echoed position was repeated as a warp"); }
  for (int n = 0; n < 400; ++n) poll(-1500 + n % 300, 200 + n % 7);
  now += 30000; out = channel.observe(at(fakeHandle(0x102), -1300, 205), none, now);
  require(out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).id == decode(out[0]).id, "Echoes took the regular records' budget");
  const auto second = decode(out[0]).id;
  const auto named = poll(-1200, 300);
  require(named && decode(*named).id == second && (decode(*named).flags & 1), "An echo did not carry the latest sent shape");
  // Hidden (e.g. a game hides it): the position still follows, with the hidden state.
  now += 30000; out = channel.observe(at(fakeHandle(0x102), -1200, 300, 0), none, now);
  require(out.size() == 1 && decode(out[0]).flags == 0, "Hidden was not reported");
  const auto hidden = poll(-1100, 300);
  require(hidden && decode(*hidden).flags == 2 && decode(*hidden).id == 0 && decode(*hidden).x == 820, "A hidden cursor's echo is not a hidden state with position");
  // A new request sends the current position although it did not change.
  require(!poll(-1100, 300), "Unchanged position echoed");
  channel.echoRestart();
  require(poll(-1100, 300, cursor::echoMinimumUs).has_value(), "A new request did not send the current position");
  // The unrepresentable fallback is unaffected by echoing.
  now += 30000; channel.observe(at(fakeHandle(0x666), -1100, 300), none, now);
  require(channel.composeFallback(), "Echoing disabled the composed fallback");
  const auto composedEcho = channel.echo(at(fakeHandle(0x666), -1000, 300), now += 3000);
  require(composedEcho && decode(*composedEcho).flags == 2 && channel.composeFallback(), "An echo during the fallback changed it or claimed a shape");
  const auto counted = channel.takeCounters();
  require(counted.echoes >= 400 && counted.warps == 0 && channel.takeCounters().echoes == 0, "Echo counter");
  // Independent outputs never echo.
  cursor::Channel outputs(std::vector<RECT>{{0, 0, 800, 600}, {800, 0, 1600, 600}}, [&] { return ids.next("echo-outputs"); }, fake.extractor());
  outputs.keyFrame(); now += 30000; outputs.observe(at(fakeHandle(0x101), 10, 10), none, now);
  require(!outputs.echo(at(fakeHandle(0x101), 20, 20), now + 5000), "Independent outputs echoed a position");
}

// capture_follow.hpp: an application changed the output's mode under the
// stream (here 2560x1440 -> 1920x1080, a game's fullscreen). Positions are
// scaled into the announced 2560x1440, which the relay bounds them by; shapes
// and the sent state stay; the next echo resends the position; back at the
// announced size they map 1:1 again.
void channelRebase() {
  const auto same = [](POINT a, POINT b) { return a.x == b.x && a.y == b.y; };
  require(same(cursor::logicalPosition(POINT{960, 540}, 1920, 1080, 2560, 1440), POINT{1280, 720}) &&
          same(cursor::logicalPosition(POINT{1919, 1079}, 1920, 1080, 2560, 1440), POINT{2558, 1438}) &&
          same(cursor::logicalPosition(POINT{2559, 1439}, 2560, 1440, 1920, 1080), POINT{1919, 1079}) &&
          same(cursor::logicalPosition(POINT{5, 6}, 100, 100, 100, 100), POINT{5, 6}) &&
          same(cursor::logicalPosition(POINT{5, 6}, 100, 100, 0, 0), POINT{5, 6}), "Logical position mapping");
  Fake fake; fake.table[0x101] = synthetic(1);
  cursor::SessionShapeIds ids; cursor::InjectedPointer::Mark none; uint64_t now = 4000000;
  cursor::Channel channel(RECT{0, 0, 2560, 1440}, [&] { return ids.next("rebase"); }, fake.extractor());
  channel.keyFrame(); now += 30000;
  const auto first = channel.observe(at(fakeHandle(0x101), 1000, 500), none, now);
  require(first.size() == 2, "Fixture state");
  const auto echoed = channel.echo(at(fakeHandle(0x101), 1000, 500), now += 5000);
  require(echoed && decode(*echoed).x == 1000 && decode(*echoed).y == 500, "Echo before the mode change");
  channel.rebase(RECT{0, 0, 1920, 1080}, 2560, 1440);
  const auto scaled = channel.echo(at(fakeHandle(0x101), 960, 540), now += 5000);
  require(scaled && decode(*scaled).x == 1280 && decode(*scaled).y == 720 && decode(*scaled).id == decode(first[0]).id && (decode(*scaled).flags & 1), "An echo after the mode change was not in the announced size");
  require(!channel.echo(at(fakeHandle(0x101), 1920, 500), now += 5000), "A position beyond the new output was echoed");
  // A foreign move (warp) after the change is scaled the same way.
  cursor::Channel warping(RECT{0, 0, 2560, 1440}, [&] { return ids.next("rebase-warp"); }, fake.extractor());
  warping.keyFrame(); now += 30000; warping.observe(at(fakeHandle(0x101), 100, 100), none, now);
  warping.rebase(RECT{0, 0, 1920, 1080}, 2560, 1440);
  std::optional<Decoded> warp;
  for (LONG x : {300L, 300L, 600L, 600L, 600L}) { now += 120000; for (const auto& r : warping.observe(at(fakeHandle(0x101), x, 300), none, now)) if (r[0] == 2 && (r[1] & 2)) warp = decode(r); }
  require(warp && warp->x == 800 && warp->y == 400, "A warp after the mode change was not in the announced size");
  // Back at the announced size: 1:1.
  channel.rebase(RECT{0, 0, 2560, 1440}, 2560, 1440);
  const auto back = channel.echo(at(fakeHandle(0x101), 2000, 1000), now += 5000);
  require(back && decode(*back).x == 2000 && decode(*back).y == 1000, "Positions after the mode returned were scaled");
  bool refused = false; try { channel.rebase(RECT{0, 0, 0, 100}, 2560, 1440); } catch (const std::runtime_error&) { refused = true; }
  require(refused, "An empty output was accepted");
  refused = false; try { channel.rebase(RECT{0, 0, 1920, 1080}, 0, 1440); } catch (const std::runtime_error&) { refused = true; }
  require(refused, "An empty announced size was accepted");
  cursor::Channel outputs(std::vector<RECT>{{0, 0, 800, 600}, {800, 0, 1600, 600}}, [&] { return ids.next("rebase-outputs"); }, fake.extractor());
  refused = false; try { outputs.rebase(RECT{0, 0, 800, 600}, 800, 600); } catch (const std::runtime_error&) { refused = true; }
  require(refused, "Independent outputs were rebased");
}

// The capture thread and the poller share one channel: with both running
// (new shapes every tick, echoes every poll), no state ever names a shape that
// had not left before it, in the order the records were handed on.
void echoSharedOrdering() {
  Fake fake; for (uintptr_t n = 1; n <= 300; ++n) fake.table[0x1000 + n] = synthetic(uint8_t(n));
  cursor::SessionShapeIds ids;
  cursor::SharedChannel channel(RECT{0, 0, 1920, 1080}, cursor::Channel::Allocate([&] { return ids.next("shared"); }), fake.extractor());
  std::vector<std::vector<uint8_t>> wire;  // appended only under the channel's lock
  const auto emit = [&](const std::vector<uint8_t>& record) { wire.push_back(record); };
  std::atomic<uint32_t> request{1}; std::atomic<uint64_t> echoes{0};
  std::atomic<LONG> position{100};
  {
    cursor::EchoPoller poller(request, nullptr, [&] { return at(fakeHandle(0x1001), position.load(), 200); },
                              [&](const cursor::Observation& seen, bool fresh) { channel.echo(seen, fresh, hostMonotonicUs(), [&](const std::vector<uint8_t>& r) { emit(r); ++echoes; }); }, 500);
    channel.keyFrame();
    // 1.2 s of capture ticks, a new shape and position on each (the shape
    // budget lets about 10 a second leave; the rest wait under reserved ids).
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    for (uintptr_t n = 1; std::chrono::steady_clock::now() < until; ++n) {
      position = LONG(100 + n % 1000);
      channel.observe(at(fakeHandle(0x1000 + 1 + n % 300), LONG(100 + n % 1000), 200), {}, hostMonotonicUs(), emit);
      std::this_thread::sleep_for(std::chrono::microseconds(700));
    }
  }
  std::set<uint32_t> shapes; unsigned states = 0;
  for (const auto& record : wire) {
    const auto value = decode(record);
    if (value.type == 1) shapes.insert(value.id);
    else { ++states; if (value.flags & 1) require(shapes.contains(value.id), "A state left before the shape it names"); }
  }
  require(echoes > 20 && shapes.size() >= 5 && states > echoes, "The shared run did not exercise both threads");
}

// The poller's own thread: nothing read while unasked, a fresh first reading
// per request, the cadence while asked (reported, not asserted tightly: this
// is a busy VM), quiet again when the request ends, joined on destruction.
std::string echoCadence;
void echoPollerCadence() {
  std::atomic<uint32_t> request{0}; HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(wake != nullptr, "Wake event");
  std::mutex mutex; std::vector<uint64_t> times; std::vector<bool> fresh; std::atomic<unsigned> reads{0};
  {
    cursor::EchoPoller poller(request, wake, [&] { ++reads; return at(fakeHandle(0x101), 1, 1); },
                              [&](const cursor::Observation&, bool first) { std::lock_guard lock(mutex); times.push_back(hostMonotonicUs()); fresh.push_back(first); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    require(reads == 0 && poller.polls() == 0, "The poller read the cursor while unasked");
    request = 7; SetEvent(wake);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    request = 0; SetEvent(wake);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto stopped = reads.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    require(reads == stopped, "The poller kept reading after the request ended");
    request = 8; SetEvent(wake);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  CloseHandle(wake);
  std::lock_guard lock(mutex);
  require(times.size() > 100, "Too few polls while asked");
  require(fresh.front() && std::count(fresh.begin(), fresh.end(), true) == 2, "Each request did not start with exactly one fresh reading");
  std::vector<uint64_t> gaps; for (size_t n = 1; n < times.size(); ++n) if (!fresh[n]) gaps.push_back(times[n] - times[n - 1]);
  std::sort(gaps.begin(), gaps.end());
  const auto q = [&](double p) { return gaps[std::min(gaps.size() - 1, size_t(p * double(gaps.size())))]; };
  require(q(0.0) >= 400, "Polls ran closer than the timer allows");
  std::ostringstream log; log << "\"echoPollUs\":{\"interval\":" << cursor::echoPollUs << ",\"samples\":" << gaps.size() << ",\"p50\":" << q(0.5) << ",\"p95\":" << q(0.95) << ",\"p99\":" << q(0.99) << ",\"max\":" << gaps.back() << '}';
  echoCadence = log.str();
}

// The production composition switch + encode policy: the direct path's cursor
// patch over a static frame. Composed, every cursor move is a changed frame;
// separated (what Capture does with composeCursor(false)), none is.
struct StaticFrame {
  int width, height; std::vector<uint8_t> bgra, nv12;
  StaticFrame(int w, int h) : width(w), height(h), bgra(size_t(w) * h * 4), nv12(size_t(w) * h * 3 / 2) {
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      auto* p = bgra.data() + (size_t(y) * w + x) * 4;
      const bool glyph = (y % 24) < 14 && (x % 9) < 6 && ((x * 7 + y * 3) % 11) < 6 && y > 60;  // text-like rows
      p[0] = glyph ? 30 : uint8_t(200 + x * 40 / w); p[1] = glyph ? 30 : uint8_t(205 + y * 30 / h); p[2] = glyph ? 30 : 225; p[3] = 255;
    }
    bgraToNv12(bgra.data(), nv12.data(), w, h);
  }
  template<class F> void visit(const capture::PixelRect& rect, F&& f) { f(bgra.data() + size_t(rect.top) * width * 4 + size_t(rect.left) * 4, size_t(width) * 4, rect); }
};
POINT pathPoint(int tick, int width, int height) {
  const double angle = tick * 0.21;
  return {LONG(width / 2 + std::cos(angle) * width / 5), LONG(height / 2 + std::sin(angle) * height / 5)};
}
void compositionSwitchAndPolicy() {
  StaticFrame frame(1280, 720); HDC screen = GetDC(nullptr); require(screen != nullptr, "Screen DC unavailable");
  const auto arrow = LoadCursorW(nullptr, IDC_ARROW);
  for (const bool separated : {false, true}) {
    auto nv12 = frame.nv12; capture::Nv12CursorPatch patch; require(patch.reserve(64, 64, screen), "Cursor canvas");
    FrameEncodePolicy policy(nv12.size()); unsigned changed = 0;
    for (int tick = 0; tick < 120; ++tick) {
      std::optional<capture::CursorDraw> cursorDraw;
      if (!separated) { const auto p = pathPoint(tick, frame.width, frame.height); cursorDraw = capture::CursorDraw{arrow, p.x, p.y, 32, 32}; }
      require(patch.apply(nv12, frame.width, frame.height, cursorDraw, [&](const capture::PixelRect& rect, auto&& f) { frame.visit(rect, f); }, screen), "Cursor composition failed");
      const uint64_t pts = uint64_t(tick) * 16667 + 1;
      const auto decision = policy.prepare(nv12, pts);
      if (decision.submit) require(policy.output(pts, tick == 0, pts, 1000), "Policy output");
      if (tick > 0 && decision.contentChanged) ++changed;
    }
    if (separated) require(changed == 0 && nv12 == frame.nv12 && !patch.patch(), "A separated session's cursor move changed the frame");
    else require(changed == 119, "A composed (legacy) cursor move did not change every frame");
  }
  ReleaseDC(nullptr, screen);
}

// The actual worker Agent control path (substituted injector, no OS input):
// negotiation, the injected-position mark, and input-latency move tagging.
const std::string session = "00000000-0000-4000-8000-0000000000c1";
std::string start(const std::string& extra) {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\",\"inputLatencyProtocol\":\"input-latency-v1\"" + extra + '}';
}
// host-resume-v1: the service's key frame request after a resumed broker
// channel carries "cursor":true; only that one makes the worker's capture loop
// send its shapes and state again (Channel::pictureRestart, tested above). An
// ordinary key frame request (dropped video) and a new stream do not.
void workerCursorResend() {
  const RECT area{0, 0, 1920, 1080};
  Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [](INPUT&) -> UINT { return 1U; }, [area] { return area; });
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  require(!agent.cursorTestResend(), "A new stream asked for the cursor again");
  agent.localControl("{\"type\":\"stream.keyframe\",\"sessionId\":\"" + session + "\"}");
  require(!agent.cursorTestResend(), "An ordinary key frame request sent the cursor again");
  for (const char* marker : {"\"cursor\":false", "\"cursor\":1", "\"cursor\":\"true\""}) {
    agent.localControl("{\"type\":\"stream.keyframe\",\"sessionId\":\"" + session + "\"," + marker + "}");
    require(!agent.cursorTestResend(), "A malformed cursor marker sent the cursor again");
  }
  agent.localControl("{\"type\":\"stream.keyframe\",\"sessionId\":\"other\",\"cursor\":true}");
  require(!agent.cursorTestResend(), "Another session's request sent the cursor again");
  agent.localControl("{\"type\":\"stream.keyframe\",\"sessionId\":\"" + session + "\",\"cursor\":true}");
  require(agent.cursorTestResend(), "The resumed channel's key frame request did not ask for the cursor again");
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  require(!agent.cursorTestResend(), "The request outlived its stream");
  agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
}

// System input (system_input.hpp): an input the service marked was already written to its SYSTEM injector. The
// worker injects nothing for it and keeps its bookkeeping: the cursor mark follows it exactly as after the
// worker's own injection, so the cursor channel does not report the injector's move as an application warp.
void workerSystemInputMarker() {
  const RECT area{100, 200, 100 + 1921, 200 + 1081};
  int injections = 0;
  Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [&injections](INPUT&) -> UINT { ++injections; return 1U; }, [area] { return area; });
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  agent.localControl(R"({"type":"input","event":"move","x":0.5,"y":0.25,"systemInput":true})");
  require(injections == 0, "A marked input was injected by the worker as well");
  auto mark = agent.cursorTestMark();
  require(mark.known && mark.position.x == 100 + 960 && mark.position.y == 200 + 270, "A marked move did not set the cursor mark the cursor channel compares with");
  agent.localControl(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"systemInput":true})");
  agent.localControl(R"({"type":"input","event":"button","button":"left","down":true,"x":0.5,"y":0.5,"systemInput":true})");
  agent.localControl(R"({"type":"input","event":"wheel","delta":120,"x":0.5,"y":0.5,"systemInput":true})");
  agent.localControl(R"({"type":"input.reset","systemInput":true})");
  require(injections == 0, "Marked keys, buttons, wheel or a reset reached the worker's injector");
  // Without the marker (no injector, or it is gone) the worker injects as it always did.
  agent.localControl(R"({"type":"input","event":"move","x":0.25,"y":0.25,"systemInput":false})");
  require(injections == 1, "An input marked false was not injected by the worker");
  agent.localControl(R"({"type":"input","event":"move","x":0.75,"y":0.75})");
  require(injections == 2, "An unmarked input was not injected by the worker");
  mark = agent.cursorTestMark();
  require(mark.known && mark.position.x == 100 + 1440 && mark.position.y == 200 + 810, "The worker's own injection no longer set the cursor mark");
  agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
}

void workerNegotiationAndInput() {
  const RECT area{100, 200, 100 + 1921, 200 + 1081};
  for (const auto& [extra, expected] : std::vector<std::pair<std::string, bool>>{{"", false}, {",\"cursorProtocol\":\"cursor-v1\"", true}, {",\"cursorProtocol\":\"cursor-v2\"", false},
       {",\"cursorProtocol\":\"cursor-v1\",\"mediaProtocol\":\"tracks-v1\"", false}}) {
    Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [](INPUT&) -> UINT { return 1U; }, [area] { return area; });
    agent.inputLatencyTestStream(JsonParser::parse(start(extra)), area);
    require(agent.cursorTestSeparated() == expected, "Cursor separation did not follow the exact session.start marker");
    agent.localControl(R"({"type":"input","event":"move","x":0.5,"y":0.25})");
    const auto mark = agent.cursorTestMark();
    require(mark.known == expected, "The injected position mark did not follow negotiation");
    if (expected) require(mark.position.x == 100 + 960 && mark.position.y == 200 + 270, "The injected position mark is not the mapped hotspot");
    agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
  }
  {
    const auto rdp = JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"rdp\",\"sessionId\":\"x\",\"cursorProtocol\":\"cursor-v1\"}");
    require(!cursor::sessionRequested(rdp), "RDP enabled cursor separation");
  }
  {
    // cursor-tracks-v1: independent outputs only with both exact markers.
    const auto track = [](const std::string& extra) { return JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"x\",\"mediaProtocol\":\"tracks-v1\"" + extra + '}'); };
    const std::string both = ",\"cursorProtocol\":\"cursor-v1\",\"cursorTracksProtocol\":\"cursor-tracks-v1\"";
    require(cursor::trackSessionRequested(track(both)) && !cursor::sessionRequested(track(both)), "Independent outputs did not separate the cursor with both markers");
    for (const auto& extra : {std::string(), std::string(",\"cursorProtocol\":\"cursor-v1\""), std::string(",\"cursorTracksProtocol\":\"cursor-tracks-v1\""),
         std::string(",\"cursorProtocol\":\"cursor-v1\",\"cursorTracksProtocol\":\"cursor-tracks-v2\""), std::string(",\"cursorProtocol\":\"cursor-v2\",\"cursorTracksProtocol\":\"cursor-tracks-v1\"")})
      require(!cursor::trackSessionRequested(track(extra)), "Independent outputs separated the cursor without both exact markers");
    require(!cursor::trackSessionRequested(JsonParser::parse(start(both))), "An ordinary stream took the independent-outputs grant");
    require(!cursor::trackSessionRequested(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"rdp\",\"sessionId\":\"x\",\"mediaProtocol\":\"tracks-v1\"" + both + '}')), "RDP took the independent-outputs grant");
  }
  // A failed injection or a bare (coordinate-free) event leaves the mark.
  Agent failing([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [](INPUT&) -> UINT { return 0U; }, [area] { return area; });
  failing.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  failing.localControl(R"({"type":"input","event":"move","x":0.5,"y":0.25})");
  require(!failing.cursorTestMark().known, "A failed injection marked a cursor position");
  Agent bare([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [](INPUT&) -> UINT { return 1U; }, [area] { return area; });
  bare.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  bare.localControl(R"({"type":"input","event":"button","button":"left","down":true})");
  bare.localControl(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})");
  require(!bare.cursorTestMark().known, "A coordinate-free event marked a cursor position");
  bare.localControl(R"({"type":"input","event":"wheel","delta":120,"x":1,"y":1})");
  require(bare.cursorTestMark().known && bare.cursorTestMark().position.x == 100 + 1920 && bare.cursorTestMark().position.y == 200 + 1080, "A positioned wheel did not mark its position");
  // input-latency-v1: under cursor-v1 a sampled move is acknowledged but is
  // never the frame candidate; a sampled button still is. Legacy unchanged.
  for (const bool separated : {false, true}) {
    std::vector<std::string> texts; std::mutex mutex;
    Agent agent([](const std::wstring&) {}, [&](bool text, std::vector<uint8_t> bytes) { std::lock_guard lock(mutex); if (text) texts.emplace_back(bytes.begin(), bytes.end()); }, [](INPUT&) -> UINT { return 1U; }, [area] { return area; });
    agent.inputLatencyTestStream(JsonParser::parse(start(separated ? ",\"cursorProtocol\":\"cursor-v1\"" : "")), area);
    auto& latency = agent.inputLatencyTestState();
    const auto received = hostMonotonicUs();
    agent.localControl(R"({"type":"input","event":"move","x":0.5,"y":0.5,"sample":1,"latencyReceivedUs":)" + std::to_string(received) + "}");
    latency.beginCapture(hostMonotonicUs() + 10); latency.submitted(5000, true);
    const bool moveTagged = latency.output(5000, 0).has_value();
    require(moveTagged == !separated, "A cursor-v1 move was tagged (or a composed move was not)");
    agent.localControl(R"({"type":"input","event":"button","button":"left","down":true,"x":0.5,"y":0.5,"sample":2,"latencyReceivedUs":)" + std::to_string(received) + "}");
    latency.beginCapture(hostMonotonicUs() + 10); latency.submitted(6000, true);
    const auto tag = latency.output(6000, 0);
    require(tag && JsonParser::parse(*tag).get("sample").number() == 2, "A button frame tag was lost");
    std::lock_guard lock(mutex);
    require(std::count_if(texts.begin(), texts.end(), [](const std::string& t) { return t.find("worker.input.latency") != std::string::npos; }) == 2, "Move/button acknowledgements changed");
    agent.localControl(R"({"type":"input","event":"button","button":"left","down":false,"x":0.5,"y":0.5})");
  }
}

// cursor-echo-v1 on the actual worker Agent control path: only a granted
// session takes the request; off, an invalid request, the session's end and
// another session clear it; a restart of the same session (resize) keeps it;
// a Session0 Agent hands it to its worker.
void workerEchoRequests() {
  const RECT area{0, 0, 1920, 1080};
  const std::string granted = ",\"cursorProtocol\":\"cursor-v1\",\"cursorEchoProtocol\":\"cursor-echo-v1\"";
  const auto on = std::string(R"({"type":"cursor.echo","on":true})"), off = std::string(R"({"type":"cursor.echo","on":false})");
  Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [](INPUT&) -> UINT { return 1U; }, [area] { return area; });
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  agent.localControl(on);
  require(!agent.cursorTestEchoGranted() && agent.cursorTestEchoRequest() == 0, "An ungranted session took an echo request");
  agent.inputLatencyTestStream(JsonParser::parse(start(granted)), area);
  require(agent.cursorTestEchoGranted() && agent.cursorTestEchoRequest() == 0, "A granted session started echoing unasked");
  agent.localControl(on);
  const auto first = agent.cursorTestEchoRequest();
  require(first != 0, "A granted echo request was not taken");
  agent.localControl(R"({"type":"cursor.echo","on":"false"})");
  agent.localControl(R"({"type":"cursor.echo","on":false,"extra":1})");
  require(agent.cursorTestEchoRequest() == first, "An invalid request changed the echo");
  agent.localControl("{\"type\":\"cursor.echo\",\"on\":false,\"sessionId\":\"other\"}");
  require(agent.cursorTestEchoRequest() == first, "Another session's request changed the echo");
  agent.localControl(off);
  require(agent.cursorTestEchoRequest() == 0, "echo off was not taken");
  agent.localControl(on);
  require(agent.cursorTestEchoRequest() != 0 && agent.cursorTestEchoRequest() != first, "A new request did not get a new number");
  // A restart of the same session (a resize) keeps the request.
  agent.inputLatencyTestStream(JsonParser::parse(start(granted)), area);
  require(agent.cursorTestEchoRequest() != 0, "A restart of the same session dropped the echo");
  // The session's end clears it.
  agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
  require(agent.cursorTestEchoRequest() == 0, "The session's end kept the echo");
  agent.inputLatencyTestStream(JsonParser::parse(start(granted)), area);
  agent.localControl(on);
  require(agent.cursorTestEchoRequest() != 0, "Echo after a new start was not taken");
  // Another session starts without it.
  agent.inputLatencyTestStream(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"00000000-0000-4000-8000-0000000000c2\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\"" + granted + '}'), area);
  require(agent.cursorTestEchoRequest() == 0, "Another session inherited the echo");
  // A session without the grant after a granted one: cleared and refused.
  agent.localControl(on);
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  agent.localControl(on);
  require(agent.cursorTestEchoRequest() == 0 && !agent.cursorTestEchoGranted(), "An ungranted restart kept or took the echo");
  // Session0: the Agent hands the request to its worker unchanged.
  struct Bridge final : AgentBridge {
    std::vector<Json> controls;
    void start(const Json&, Send) override {}
    void control(const Json& value) override { controls.push_back(value); }
    void stop() override {}
    std::string capabilities() override { return "{\"service\":true}"; }
    void displayControl(const Json&, Send) override {}
  } bridge;
  Agent service([](const std::wstring&) {}, &bridge); service.publicationTestOpen();
  service.localControl(start(granted));
  service.localControl(on);
  require(bridge.controls.size() == 1 && bridge.controls[0].get("type").string() == "cursor.echo" && bridge.controls[0].get("on").boolean(false), "The service did not hand the echo request to its worker");
}

// cursor-compose-v1: the session marker and the client's request.
void composeGatingAndRequests() {
  const auto parse = [](const std::string& extra, const char* mode = "stream") { return JsonParser::parse(std::string("{\"type\":\"session.start\",\"mode\":\"") + mode + "\",\"sessionId\":\"x\"" + extra + '}'); };
  const std::string both = ",\"cursorProtocol\":\"cursor-v1\",\"cursorComposeProtocol\":\"cursor-compose-v1\"";
  require(std::string(cursor::composeProtocol) == "cursor-compose-v1", "Compose protocol marker changed");
  require(cursor::composeSessionRequested(parse(both)), "A granted compose session was not recognized");
  require(!cursor::composeSessionRequested(parse(",\"cursorComposeProtocol\":\"cursor-compose-v1\"")), "Compose without cursor-v1 was granted");
  require(!cursor::composeSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\"")), "cursor-v1 alone granted compose");
  require(!cursor::composeSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\",\"cursorEchoProtocol\":\"cursor-echo-v1\"")), "The echo grant granted compose");
  require(!cursor::composeSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\",\"cursorComposeProtocol\":\"cursor-compose-v2\"")), "Another compose value was granted");
  require(!cursor::composeSessionRequested(parse(",\"cursorProtocol\":\"cursor-v1\",\"cursorComposeProtocol\":true")), "A non-string compose marker was granted");
  require(!cursor::composeSessionRequested(parse(both + ",\"mediaProtocol\":\"tracks-v1\"")), "Independent outputs were granted compose");
  require(!cursor::composeSessionRequested(parse(both, "rdp")), "RDP was granted compose");
  require(cursor::composeRequest(JsonParser::parse(R"({"type":"cursor.compose","on":true})")) == std::optional<bool>(true), "compose on refused");
  require(cursor::composeRequest(JsonParser::parse(R"({"type":"cursor.compose","on":false,"sessionId":"x"})")) == std::optional<bool>(false), "compose off with sessionId refused");
  for (const char* bad : {R"({"type":"cursor.compose"})", R"({"type":"cursor.compose","on":1})", R"({"type":"cursor.compose","on":"true"})", R"({"type":"cursor.compose","on":true,"x":2})", R"({"type":"cursor.echo","on":true})", R"([true])"})
    require(!cursor::composeRequest(JsonParser::parse(bad)), "An invalid compose request was accepted");
}

// cursor-compose-v1 on the channel: while asked, every tick composes and the
// client is told hidden (it must never draw a second cursor); no shape is read
// or published; resting positions still leave; the composed cursor follows
// Windows' visibility (nothing drawn while an application hides it). Off: the
// separated cursor returns with its shape at once and composition stops; the
// unrepresentable fallback is unchanged.
void channelCompose() {
  Fake fake; fake.table[0x101] = synthetic(1); fake.table[0x102] = synthetic(2); fake.table[0x666] = std::nullopt;
  cursor::SessionShapeIds ids; const RECT source{-1920, 0, 0, 1080};
  cursor::Channel channel(source, [&] { return ids.next("compose"); }, fake.extractor()); channel.keyFrame();
  cursor::InjectedPointer::Mark none; uint64_t now = 3000000;
  const auto tick = [&](uintptr_t handle, LONG x, LONG y, DWORD flags = CURSOR_SHOWING) { now += 30000; return channel.observe(at(fakeHandle(handle), x, y, flags), none, now); };
  auto out = tick(0x101, -1000, 500);
  require(out.size() == 2 && decode(out[1]).flags == 1 && !channel.composeFallback(), "Not separated before the request");
  const auto first = decode(out[0]).id;
  (void)channel.takeCounters();
  channel.composeRequested(true);
  out = tick(0x101, -1000, 500);
  require(channel.composeFallback() && out.size() == 1 && decode(out[0]).type == 2 && decode(out[0]).flags == 0 && decode(out[0]).id == 0, "A compose request did not compose and report hidden");
  require(capture::drawsCursor(channel.composeFallback(), true, CURSOR_SHOWING), "A shown cursor is not drawn while composing");
  const auto reads = fake.extractions;
  for (int n = 1; n <= 30; ++n)
    for (const auto& record : tick(n % 2 ? 0x102 : 0x101, -1000 + n * 3, 500)) require(record[0] == 2 && !(record[1] & 1), "A composing session published a shape or a visible state");
  require(fake.extractions == reads && channel.composeFallback(), "Composing read shapes or stopped");
  // A game hides it: composition stays on, the capture draws nothing, the client still sees hidden.
  out = tick(0x101, -900, 500, 0);
  require(channel.composeFallback() && (out.empty() || !(decode(out[0]).flags & 1)), "A hidden cursor changed composition or became visible");
  require(!capture::drawsCursor(channel.composeFallback(), true, 0) && !capture::drawsCursor(channel.composeFallback(), true, CURSOR_SUPPRESSED) && !capture::drawsCursor(channel.composeFallback(), false, CURSOR_SHOWING),
          "The composed cursor did not follow Windows' visibility");
  require(!capture::drawsCursor(false, true, CURSOR_SHOWING), "A separated cursor was drawn");
  // At rest the position still leaves (hidden state with position).
  tick(0x101, -850, 450);
  out = tick(0x101, -850, 450);
  require(out.size() == 1 && decode(out[0]).flags == 2 && decode(out[0]).id == 0 && decode(out[0]).x == 1070 && decode(out[0]).y == 450, "A resting position was not reported while composing");
  const auto counted = channel.takeCounters();
  require(counted.composed == 0 && counted.shapes == 0, "Compose ticks were counted as the fallback or shapes were sent");
  // Off: the separated cursor returns at once (a shape first seen while composing is published first).
  channel.composeRequested(false);
  out = tick(0x102, -800, 400);
  require(!channel.composeFallback() && out.size() == 2 && decode(out[0]).type == 1 && decode(out[1]).flags == 1 && decode(out[1]).id == decode(out[0]).id, "Off did not bring back the separated cursor with its shape");
  require(!capture::drawsCursor(channel.composeFallback(), true, CURSOR_SHOWING), "The cursor was still drawn after off");
  out = tick(0x101, -800, 400);
  require(out.size() == 1 && decode(out[0]).id == first && (decode(out[0]).flags & 1), "A cached shape was not named after compose");
  // The unrepresentable fallback works as before after a request ended.
  out = tick(0x666, -800, 400);
  require(channel.composeFallback() && out.size() == 1 && decode(out[0]).flags == 0, "The fallback changed after compose");
  out = tick(0x101, -800, 400);
  require(!channel.composeFallback() && out.size() == 1 && (decode(out[0]).flags & 1), "The fallback did not end");
  // Through the shared channel as the capture thread uses it.
  cursor::SharedChannel shared(source, cursor::Channel::Allocate([&] { return ids.next("compose-shared"); }), fake.extractor());
  shared.keyFrame(); shared.composeRequested(true);
  std::vector<std::vector<uint8_t>> emitted;
  shared.observe(at(fakeHandle(0x101), -500, 300), none, now += 30000, [&](const std::vector<uint8_t>& record) { emitted.push_back(record); });
  require(shared.composeFallback() && emitted.size() == 1 && decode(emitted[0]).flags == 0, "The shared channel did not compose");
}

// cursor-compose-v1 on the actual worker Agent control path: only a granted
// session takes the request; invalid requests, another session, the session's
// end and an ungranted restart clear or ignore it; a restart of the same
// session keeps it; a Session0 Agent hands it to its worker.
void workerComposeRequests() {
  const RECT area{0, 0, 1920, 1080};
  const std::string granted = ",\"cursorProtocol\":\"cursor-v1\",\"cursorComposeProtocol\":\"cursor-compose-v1\"";
  const auto on = std::string(R"({"type":"cursor.compose","on":true})"), off = std::string(R"({"type":"cursor.compose","on":false})");
  Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [](INPUT&) -> UINT { return 1U; }, [area] { return area; });
  const auto& compose = agent.cursorTestCompose();
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\",\"cursorEchoProtocol\":\"cursor-echo-v1\"")), area);
  agent.localControl(on);
  require(!compose.granted() && !compose.wanted(), "An echo-only session took a compose request");
  agent.inputLatencyTestStream(JsonParser::parse(start(granted)), area);
  require(compose.granted() && !compose.wanted(), "A granted session started composing unasked");
  agent.localControl(on);
  require(compose.wanted(), "A granted compose request was not taken");
  agent.localControl(R"({"type":"cursor.compose","on":"false"})");
  agent.localControl(R"({"type":"cursor.compose","on":false,"extra":1})");
  agent.localControl(R"({"type":"cursor.echo","on":false})");
  require(compose.wanted(), "An invalid request or the echo changed compose");
  agent.localControl("{\"type\":\"cursor.compose\",\"on\":false,\"sessionId\":\"other\"}");
  require(compose.wanted(), "Another session's request changed compose");
  agent.localControl(off);
  require(!compose.wanted(), "compose off was not taken");
  agent.localControl(on);
  agent.inputLatencyTestStream(JsonParser::parse(start(granted)), area);
  require(compose.wanted(), "A restart of the same session dropped compose");
  agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
  require(!compose.wanted(), "The session's end kept compose");
  agent.inputLatencyTestStream(JsonParser::parse(start(granted)), area);
  agent.localControl(on);
  require(compose.wanted(), "Compose after a new start was not taken");
  agent.inputLatencyTestStream(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"00000000-0000-4000-8000-0000000000c3\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\"" + granted + '}'), area);
  require(compose.granted() && !compose.wanted(), "Another session inherited compose");
  agent.localControl(on);
  agent.inputLatencyTestStream(JsonParser::parse(start(",\"cursorProtocol\":\"cursor-v1\"")), area);
  agent.localControl(on);
  require(!compose.granted() && !compose.wanted(), "An ungranted restart kept or took compose");
  struct Bridge final : AgentBridge {
    std::vector<Json> controls;
    void start(const Json&, Send) override {}
    void control(const Json& value) override { controls.push_back(value); }
    void stop() override {}
    std::string capabilities() override { return "{\"service\":true}"; }
    void displayControl(const Json&, Send) override {}
  } bridge;
  Agent service([](const std::wstring&) {}, &bridge); service.publicationTestOpen();
  service.localControl(start(granted));
  service.localControl(on);
  require(bridge.controls.size() == 1 && bridge.controls[0].get("type").string() == "cursor.compose" && bridge.controls[0].get("on").boolean(false), "The service did not hand the compose request to its worker");
}

// Synthetic CPU measurement on the production path: a static 1920x1080
// desktop, the cursor moved every tick of a simulated 60 fps clock, composed by
// the production direct-path patch (legacy) or left out (cursor-v1), then the
// production FrameEncodePolicy (with its CBR refinement) and the Microsoft
// software H.264 MFT. Seconds 1-4 are measured (the first IDR and its
// refinement are excluded).
struct PhaseResult { unsigned submitted = 0, encoded = 0, changed = 0, refresh = 0, refine = 0; double encodeMs = 0, cpuMs = 0, bytes = 0; };
double processCpuMs() { FILETIME created, exited, kernel, user; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user); const auto value = [](FILETIME t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000.0; }; return value(kernel) + value(user); }
PhaseResult measure(bool separated, std::ostream& log) {
  constexpr int width = 1920, height = 1080, fps = 60, bitrate = 12000000, ticks = 300, warmup = 60;
  StaticFrame frame(width, height); HDC screen = GetDC(nullptr); require(screen != nullptr, "Screen DC unavailable");
  const auto arrow = LoadCursorW(nullptr, IDC_ARROW);
  auto nv12 = frame.nv12; capture::Nv12CursorPatch patch; require(patch.reserve(64, 64, screen), "Cursor canvas");
  Encoder encoder(width, height, fps, bitrate); FrameEncodePolicy policy(nv12.size(), FrameRefinement::forCbr(bitrate, fps));
  PhaseResult result; double cpuStart = 0;
  for (int tick = 0; tick < ticks; ++tick) {
    if (tick == warmup) cpuStart = processCpuMs();
    std::optional<capture::CursorDraw> cursorDraw;
    if (!separated) { const auto p = pathPoint(tick, width, height); cursorDraw = capture::CursorDraw{arrow, p.x, p.y, 32, 32}; }
    require(patch.apply(nv12, width, height, cursorDraw, [&](const capture::PixelRect& rect, auto&& f) { frame.visit(rect, f); }, screen), "Cursor composition failed");
    const uint64_t pts = uint64_t(tick) * 1000000 / fps + 1;
    const auto decision = policy.prepare(nv12, pts);
    if (!decision.submit) continue;
    const auto began = Clock::now();
    encoder.encode(nv12.data(), pts, [&](Frame output) { require(policy.output(output.ptsUs, output.key, pts, output.bytes.size()), "Encoder output did not match"); if (tick >= warmup) { ++result.encoded; result.bytes += double(output.bytes.size()); } });
    if (tick >= warmup) {
      ++result.submitted; result.encodeMs += std::chrono::duration<double, std::milli>(Clock::now() - began).count();
      result.changed += decision.contentChanged; result.refresh += decision.refresh; result.refine += decision.refine;
    }
  }
  result.cpuMs = processCpuMs() - cpuStart;
  ReleaseDC(nullptr, screen);
  const double seconds = double(ticks - warmup) / fps;
  log << std::fixed << std::setprecision(2) << '"' << (separated ? "separated" : "composed") << "\":{\"encodedFps\":" << result.encoded / seconds << ",\"submittedFps\":" << result.submitted / seconds
      << ",\"changedFps\":" << result.changed / seconds << ",\"refreshFps\":" << result.refresh / seconds << ",\"refineFps\":" << result.refine / seconds
      << ",\"encodeMsPerSecond\":" << result.encodeMs / seconds << ",\"processCpuMsPerSecond\":" << result.cpuMs / seconds << ",\"kbps\":" << result.bytes * 8 / seconds / 1000 << '}';
  return result;
}
std::string measurement;
void cpuMeasurement() {
  Runtime runtime; std::ostringstream log;
  const auto composed = measure(false, log); log << ',';
  const auto separated = measure(true, log);
  measurement = log.str();
  require(composed.changed >= 235 && composed.encoded >= 235, "The composed cursor did not drive the encoder on every tick");
  // Separated: the cursor changes nothing; only the policy's own once-per-second
  // refresh and its bounded refinement tail of the first picture remain.
  require(separated.changed == 0, "A separated cursor move still changed a frame");
  require(separated.submitted == separated.refresh + separated.refine && separated.refresh <= 5, "A separated session submitted frames beyond refresh/refinement");
  // Per-frame cost depends on the encoder (x264 codes a cursor-only change as
  // cheap skip blocks, the MFT does not), and x264 at this rate uses the
  // policy's full refinement tail for the first picture. The encoder-independent
  // claim: apart from that tail, a tenfold cut in submitted frames, and less
  // encoder time overall.
  require((separated.submitted - separated.refine) * 10 < composed.encoded && separated.encodeMs < composed.encodeMs, "Separation did not cut encoder submissions tenfold and encoder time");
}
}
int main(int argc, char** argv) {
  // The focused ordering regression uses no GDI, capture, native pointer,
  // encoder, input injection, display setting, process or network operation.
  if (argc == 2 && std::string(argv[1]) == "--channel-send-order-only") {
    try { channelPendingShapesPublishInOrder(); std::cout << "{\"kind\":\"cursor-pending-send-order\",\"passed\":true,\"checks\":" << checks << ",\"injectedExtraction\":true,\"simulatedClock\":true,\"osInputEmitted\":false,\"captureStarted\":false,\"network\":false}\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "channel-pending-shape-send-order: " << error.what() << '\n'; return 1; }
  }
  // cursor-echo-v1 only: no GDI, capture, encoder, OS input or network.
  if (argc == 2 && std::string(argv[1]) == "--echo-only") {
    const struct { const char* name; void (*run)(); } echo[] = {{"echo-gating-requests", echoGatingAndRequests}, {"channel-echo", channelEcho}, {"channel-rebase", channelRebase}, {"echo-shared-ordering", echoSharedOrdering}, {"echo-poller-cadence", echoPollerCadence}, {"worker-echo-requests", workerEchoRequests}};
    unsigned passed = 0, failed = 0;
    for (const auto& value : echo) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
    std::cout << "{\"kind\":\"cursor-echo-v1-host\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << (echoCadence.empty() ? std::string() : "," + echoCadence) << ",\"osInputEmitted\":false,\"captureStarted\":false,\"network\":false}\n";
    return failed ? 1 : 0;
  }
  // cursor-compose-v1 only: no capture, encoder, OS input or network.
  if (argc == 2 && std::string(argv[1]) == "--compose-only") {
    const struct { const char* name; void (*run)(); } compose[] = {{"compose-gating-requests", composeGatingAndRequests}, {"channel-compose", channelCompose}, {"worker-compose-requests", workerComposeRequests}};
    unsigned passed = 0, failed = 0;
    for (const auto& value : compose) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
    std::cout << "{\"kind\":\"cursor-compose-v1-host\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"osInputEmitted\":false,\"captureStarted\":false,\"network\":false}\n";
    return failed ? 1 : 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--channel-protocol-only") {
    const struct { const char* name; void (*run)(); } protocol[] = {{"channel-protocol-records", channelProtocolRecords}, {"channel-shapes-visibility", channelShapesAndVisibility},
      {"channel-cache-bounds-rates", channelCacheBoundsAndRates}, {"channel-pending-shape-send-order", channelPendingShapesPublishInOrder}, {"channel-pending-id-exhaustion", channelPendingShapeIdExhaustion}, {"channel-picture-restart", channelPictureRestart}, {"channel-warps", channelWarps}, {"channel-outputs", channelOutputs},
      {"echo-gating-requests", echoGatingAndRequests}, {"channel-echo", channelEcho}, {"channel-rebase", channelRebase}, {"echo-shared-ordering", echoSharedOrdering}};
    unsigned passed = 0, failed = 0;
    for (const auto& value : protocol) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
    std::cout << "{\"kind\":\"cursor-channel-protocol\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"injectedExtraction\":true,\"simulatedClock\":true,\"osInputEmitted\":false,\"captureStarted\":false,\"network\":false}\n";
    return failed ? 1 : 0;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const struct { const char* name; void (*run)(); } cases[] = {{"conversion-per-kind", conversionPerKind}, {"extraction-lifetime", extractionLifetime}, {"records", records}, {"channel-shapes-visibility", channelShapesAndVisibility},
    {"channel-cache-bounds-rates", channelCacheBoundsAndRates}, {"channel-pending-shape-send-order", channelPendingShapesPublishInOrder}, {"channel-pending-id-exhaustion", channelPendingShapeIdExhaustion}, {"channel-picture-restart", channelPictureRestart}, {"channel-warps", channelWarps}, {"channel-outputs", channelOutputs}, {"composition-switch-policy", compositionSwitchAndPolicy},
    {"echo-gating-requests", echoGatingAndRequests}, {"channel-echo", channelEcho}, {"channel-rebase", channelRebase}, {"echo-shared-ordering", echoSharedOrdering}, {"echo-poller-cadence", echoPollerCadence}, {"worker-echo-requests", workerEchoRequests},
    {"compose-gating-requests", composeGatingAndRequests}, {"channel-compose", channelCompose}, {"worker-compose-requests", workerComposeRequests},
    {"worker-negotiation-input", workerNegotiationAndInput}, {"worker-cursor-resend", workerCursorResend}, {"channel-hide-secure-desktop", channelHideForSecureDesktop}, {"worker-system-input-marker", workerSystemInputMarker}, {"cpu-measurement", cpuMeasurement}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"cursor-v1-host\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"renderedRgbCases\":" << renderedCases
            << ",\"measurement\":{" << measurement << "}" << (echoCadence.empty() ? std::string() : "," + echoCadence) << ",\"desktopCursorMoved\":false,\"osInputEmitted\":false,\"network\":false}\n";
  return failed ? 1 : 0;
}
