// cursor-v1: the shape sent has the size Windows shows (cursor_channel.hpp "The
// size Windows shows"). The decision on numbers; the scaled shapes of real
// cursors (the shared system cursors and cursors made in memory) at 100, 150
// and 200 %. Nothing on the desktop is changed: no display setting, no cursor
// position, no cursor scheme; cursors are only read and copied.
#include <variant>  // cursor_channel.hpp's request readers use std::get_if
#include "cursor_channel.hpp"
#include <cstdio>
#include <tuple>
#include <utility>

using namespace host;
namespace {
unsigned checks = 0, failed = 0;
void check(bool value, const char* name) { ++checks; if (!value) { ++failed; std::printf("FAIL %s\n", name); } }

struct Owned {
  HCURSOR value = nullptr;
  explicit Owned(HCURSOR handle) : value(handle) {}
  Owned(Owned&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  Owned(const Owned&) = delete;
  ~Owned() { if (value) DestroyIcon(reinterpret_cast<HICON>(value)); }
};
// A cursor made in memory (no resource to reload from): a filled square with a
// transparent border, colour with alpha or monochrome, hotspot at (w/4, h/2).
Owned make(unsigned width, unsigned height, bool monochrome) {
  const size_t stride = cursor::maskStride(width);
  std::vector<uint8_t> mask(stride * height * (monochrome ? 2 : 1), 0), colour(size_t(width) * height * 4, 0);
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
    const bool inside = x >= 2 && y >= 2 && x + 2 < width && y + 2 < height;
    if (!inside) mask[y * stride + x / 8] |= uint8_t(0x80u >> (x % 8));               // AND 1: transparent
    else if (monochrome && (x + y) % 2) mask[(y + height) * stride + x / 8] |= uint8_t(0x80u >> (x % 8));  // XOR 1 over AND 0: white
    if (inside) { auto* pixel = colour.data() + (size_t(y) * width + x) * 4; pixel[0] = 40; pixel[1] = 160; pixel[2] = 240; pixel[3] = 255; }
  }
  ICONINFO info{}; info.fIcon = FALSE; info.xHotspot = width / 4; info.yHotspot = height / 2;
  // CreateBitmap rows are word aligned; the mask rows here are dword aligned.
  std::vector<uint8_t> words; const size_t wordStride = ((size_t(width) + 15) / 16) * 2; const unsigned rows = height * (monochrome ? 2 : 1);
  words.resize(wordStride * rows);
  for (unsigned y = 0; y < rows; ++y) std::memcpy(words.data() + y * wordStride, mask.data() + y * stride, wordStride);
  info.hbmMask = CreateBitmap(int(width), int(rows), 1, 1, words.data());
  if (!monochrome) {
    BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = LONG(width); header.bV5Height = -LONG(height); header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00; header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr; const HDC screen = GetDC(nullptr);
    info.hbmColor = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (info.hbmColor && bits) std::memcpy(bits, colour.data(), colour.size());
  }
  Owned out(reinterpret_cast<HCURSOR>(CreateIconIndirect(&info)));
  if (info.hbmMask) DeleteObject(info.hbmMask);
  if (info.hbmColor) DeleteObject(info.hbmColor);
  return out;
}
// Something is drawn: an opaque colour pixel, a mask bit that is not
// "transparent", or (monochrome) an inverting pixel (AND 1, XOR 1: the I-beam).
bool visible(const cursor::Shape& shape) {
  if (shape.format == cursor::Format::colour) { for (size_t at = 3; at < shape.colour.size(); at += 4) if (shape.colour[at]) return true; }
  const size_t stride = cursor::maskStride(shape.width);
  const auto bit = [&](unsigned row, unsigned x) { return (shape.mask[row * stride + x / 8] & uint8_t(0x80u >> (x % 8))) != 0; };
  for (unsigned y = 0; y < shape.height; ++y) for (unsigned x = 0; x < shape.width; ++x)
    if (!bit(y, x) || (shape.format == cursor::Format::monochrome && bit(y + shape.height, x))) return true;
  return false;
}
cursor::ShownInputs systemAt(unsigned dpi, unsigned arrowSide) {
  cursor::ShownInputs in; in.outputDpi = dpi; in.systemCursor = true; in.arrowSide = arrowSide; in.nominalSide = 32 * dpi / 96; return in;
}
cursor::ShownInputs windowAt(unsigned dpi, int awareness, unsigned processDpi = 0) {
  cursor::ShownInputs in; in.outputDpi = dpi; in.awareness = awareness; in.processDpi = processDpi; return in;
}
bool about(double a, double b) { return a > b - 1e-9 && a < b + 1e-9; }

void decision() {
  // A session that signed in at 100 %: the system cursors' bitmaps are 32 pixels.
  check(about(cursor::shownFactor(systemAt(96, 32)), 1.0), "100 %: the bitmap is the size shown");
  check(about(cursor::shownFactor(systemAt(120, 32)), 1.25), "125 %: 40 pixels");
  check(about(cursor::shownFactor(systemAt(144, 32)), 1.5), "150 %: 48 pixels");
  check(about(cursor::shownFactor(systemAt(168, 32)), 1.75), "175 %: 56 pixels");
  check(about(cursor::shownFactor(systemAt(192, 32)), 2.0), "200 %: 64 pixels");
  // A session whose bitmaps already have the output's size is left alone.
  check(about(cursor::shownFactor(systemAt(144, 48)), 1.0) && about(cursor::shownFactor(systemAt(192, 64)), 1.0), "bitmaps of the shown size are not scaled again");
  // Signed in at 200 %, shown on an output at 100 % or 150 %: smaller.
  check(about(cursor::shownFactor(systemAt(96, 64)), 0.5) && about(cursor::shownFactor(systemAt(144, 64)), 0.75), "a larger bitmap is shown smaller");
  // Within 5 % nothing is resampled; without the arrow's size nothing is known.
  check(about(cursor::shownFactor(systemAt(100, 32)), 1.0) && about(cursor::shownFactor(systemAt(144, 0)), 1.0), "small differences and unknown sizes change nothing");
  // A window Windows scales itself: by output DPI over the DPI it draws for.
  check(about(cursor::shownFactor(windowAt(144, 0)), 1.5) && about(cursor::shownFactor(windowAt(192, 0)), 2.0) && about(cursor::shownFactor(windowAt(96, 0)), 1.0), "a DPI-unaware window's cursor is scaled like the window");
  check(about(cursor::shownFactor(windowAt(144, 1, 96)), 1.5) && about(cursor::shownFactor(windowAt(144, 1, 144)), 1.0) && about(cursor::shownFactor(windowAt(192, 1, 144)), 192.0 / 144.0), "a system-aware window's cursor is scaled from its own system DPI");
  check(about(cursor::shownFactor(windowAt(144, 1, 0)), 1.0), "an unknown process DPI changes nothing");
  // A per-monitor aware program sizes its own cursors; unknown is never scaled.
  check(about(cursor::shownFactor(windowAt(144, 2)), 1.0) && about(cursor::shownFactor(windowAt(192, 2, 96)), 1.0) && about(cursor::shownFactor(windowAt(192, -1)), 1.0), "per-monitor aware and unknown windows are sent as they are");
  check(about(cursor::shownFactor(windowAt(0, 0)), 1.0) && about(cursor::shownFactor(windowAt(5000, 0)), 1.0) && about(cursor::shownFactor(cursor::ShownInputs{}), 1.0), "an implausible DPI changes nothing");
  // Sizes: both sides by one factor, rounded, at least one pixel, never past 256.
  const auto size = [](unsigned w, unsigned h, double f) { const SIZE s = cursor::shownSize(w, h, f); return std::pair<long, long>(s.cx, s.cy); };
  check(size(32, 32, 1.0) == std::pair<long, long>(32, 32) && size(32, 32, 1.5) == std::pair<long, long>(48, 48) && size(32, 32, 2.0) == std::pair<long, long>(64, 64), "32 pixels at 100, 150 and 200 %");
  check(size(32, 32, 1.25) == std::pair<long, long>(40, 40) && size(32, 32, 1.75) == std::pair<long, long>(56, 56) && size(37, 29, 1.5) == std::pair<long, long>(56, 44), "other scales and sizes round");
  check(size(200, 100, 2.0) == std::pair<long, long>(256, 128) && size(256, 256, 2.0) == std::pair<long, long>(256, 256) && size(4, 2, 0.25) == std::pair<long, long>(1, 1), "the largest shape is 256 pixels and the smallest one");
  check(size(0, 32, 2.0) == std::pair<long, long>(0, 32) && size(32, 32, 0.0) == std::pair<long, long>(32, 32), "nothing to scale");
}

void scaled(HCURSOR handle, const char* name, bool expectShape = true) {
  const auto base = cursor::extract(handle);
  check(base.has_value() == expectShape, name);
  if (!base) return;
  const auto same = cursor::extractScaled(handle, 1.0), close = cursor::extractScaled(handle, 1.004);
  check(same && cursor::shapeHash(*same) == cursor::shapeHash(*base) && close && cursor::shapeHash(*close) == cursor::shapeHash(*base), "100 %: the cursor's own bitmap, bit for bit");
  for (const double factor : {1.5, 2.0}) {
    const auto shape = cursor::extractScaled(handle, factor);
    const SIZE want = cursor::shownSize(base->width, base->height, factor);
    check(shape.has_value(), "a scaled shape exists");
    if (!shape) continue;
    check(LONG(shape->width) == want.cx && LONG(shape->height) == want.cy, "the shape has the size Windows shows");
    check(shape->format == base->format && cursor::validShape(*shape), "the scaled shape is a valid shape of the same kind");
    // The hotspot moves with the picture (Windows rounds it its own way).
    const double hx = base->hotspotX * factor, hy = base->hotspotY * factor;
    check(shape->hotspotX + 2.0 >= hx && shape->hotspotX <= hx + 2.0 && shape->hotspotY + 2.0 >= hy && shape->hotspotY <= hy + 2.0, "the hotspot is scaled with the shape");
    check(visible(*shape), "the scaled shape draws something");
    // It goes out as an ordinary record.
    const auto record = cursor::shapeRecord(*shape, 7);
    check(record.size() == 16 + shape->mask.size() + shape->colour.size() && record[8] == uint8_t(shape->width) && record[10] == uint8_t(shape->height), "the scaled shape is an ordinary shape record");
  }
}

void shapes() {
  // The shared system cursors, as on a desktop at 100 %, 150 % and 200 %.
  for (const auto id : {IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_SIZEALL, IDC_SIZEWE, IDC_CROSS, IDC_NO, IDC_WAIT}) scaled(LoadCursorW(nullptr, id), "a system cursor is readable");
  // Cursors made in memory: nothing to reload, Windows stretches them.
  for (const auto& [width, height, monochrome] : {std::tuple<unsigned, unsigned, bool>{32, 32, false}, {32, 32, true}, {37, 29, false}, {48, 48, true}}) {
    const auto made = make(width, height, monochrome);
    scaled(made.value, "a cursor made in memory is readable");
  }
  // 200 x 100 at 200 % would be 400 pixels: the largest shape is sent instead.
  { const auto wide = make(200, 100, false); const auto shape = cursor::extractScaled(wide.value, 2.0);
    check(shape && shape->width == 256 && shape->height == 128 && cursor::validShape(*shape), "a cursor too large at that scale is capped at 256 pixels"); }
  check(!cursor::extractScaled(nullptr, 1.5) && !cursor::extractShown(nullptr), "no cursor, no shape");
}

// On this machine, at whatever scale its output has now (read only): the
// system arrow goes out at the nominal cursor size of that output.
void here() {
  const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
  const auto in = cursor::readShownInputs(arrow);
  check(in.systemCursor && in.outputDpi >= 96 && in.arrowSide > 0 && in.nominalSide > 0, "the arrow is a system cursor with a known size");
  const auto shown = cursor::extractShown(arrow);
  check(shown.has_value(), "the arrow has a shown shape");
  if (shown) {
    const double factor = cursor::shownFactor(in);
    const SIZE want = cursor::shownSize(in.arrowSide, in.arrowSide, factor);
    check(LONG(shown->width) == want.cx, "the arrow is sent at the size this output shows it");
    if (factor != 1.0) check(LONG(shown->width) + 1 >= LONG(in.nominalSide) && LONG(shown->width) <= LONG(in.nominalSide) + 1, "which is the nominal cursor size of the output's scale");
    std::printf("output %u dpi, arrow bitmap %u, nominal %u, sent %u\n", in.outputDpi, in.arrowSide, in.nominalSide, unsigned(shown->width));
  }
  // A cursor of this process (per-monitor aware as the Host worker is, or not
  // under the pointer at all): never a system cursor.
  const auto own = make(32, 32, false);
  check(!cursor::readShownInputs(own.value).systemCursor, "a made cursor is not a system cursor");
  check(cursor::extractShown(own.value).has_value(), "a made cursor has a shown shape");
}
}

int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // as the Host worker (main.cpp)
  decision(); shapes(); here();
  std::printf("checks=%u failed=%u\n", checks, failed);
  return failed ? 1 : 0;
}
