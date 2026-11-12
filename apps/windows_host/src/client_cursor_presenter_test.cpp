// cursor-v1 client presenter (apps/windows_client/windows/runner/
// cursor_presenter.cpp): strict record parsing, the shape cache, the
// WM_SETCURSOR rule on a real hidden window with faked cursor calls, scaling,
// warps and restoration. The real desktop cursor is never read or changed:
// every GetCursor/SetCursor/GetCursorPos/SetCursorPos/WindowFromPoint goes to
// the fakes below. Real HCURSORs are created and destroyed (leak checked).
#include "cursor_presenter.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// The Host's own channel (header-only), for the Host-to-presenter case below.
#include "cursor_channel.hpp"

namespace {
unsigned checks = 0, malformedCases = 0;
void require(bool ok, const std::string& reason) {
  ++checks;
  if (!ok) throw std::runtime_error(reason);
}

struct Fake {
  HCURSOR current = nullptr;
  POINT pos{};
  HWND under = nullptr;
  ULONGLONG ticks = 100000;
  UINT dpi = 96;
  POINT origin{};
  std::vector<POINT> moves;
  unsigned sets = 0;
} fake;
HCURSOR WINAPI FakeGet() { return fake.current; }
HCURSOR WINAPI FakeSet(HCURSOR cursor) { const auto previous = fake.current; fake.current = cursor; ++fake.sets; return previous; }
BOOL WINAPI FakePosition(LPPOINT point) { *point = fake.pos; return TRUE; }
BOOL WINAPI FakeMove(int x, int y) { fake.moves.push_back(POINT{x, y}); fake.pos = POINT{x, y}; return TRUE; }
HWND WINAPI FakeWindow(POINT) { return fake.under; }
ULONGLONG WINAPI FakeTicks() { return fake.ticks; }
UINT WINAPI FakeDpi(HWND) { return fake.dpi; }
BOOL WINAPI FakeToScreen(HWND, LPPOINT point) { point->x += fake.origin.x; point->y += fake.origin.y; return TRUE; }
multitap::CursorOs fakeOs() {
  multitap::CursorOs os;
  os.get = FakeGet; os.set = FakeSet; os.position = FakePosition; os.move = FakeMove;
  os.window = FakeWindow; os.ticks = FakeTicks; os.dpi = FakeDpi; os.toScreen = FakeToScreen;
  return os;
}

void put16(std::vector<uint8_t>& b, size_t at, unsigned v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
size_t stride(unsigned width) { return ((size_t(width) + 31) / 32) * 4; }
void bit(std::vector<uint8_t>& b, size_t base, unsigned width, unsigned row, unsigned x) { b[base + size_t(row) * stride(width) + x / 8] |= uint8_t(0x80u >> (x % 8)); }

// format 1: an alpha arrow-like block; format 2: AND/XOR checker incl. inverting pixels.
std::vector<uint8_t> shapeRecord(uint32_t id, uint8_t format, unsigned w, unsigned h, unsigned hx = 0, unsigned hy = 0) {
  const bool mono = format == 2;
  const size_t mask = stride(w) * h * (mono ? 2 : 1), color = mono ? 0 : size_t(w) * h * 4;
  std::vector<uint8_t> b(16 + mask + color, 0);
  b[0] = 1; b[1] = format; put32(b, 4, id); put16(b, 8, w); put16(b, 10, h); put16(b, 12, hx); put16(b, 14, hy);
  for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
    if (mono) {
      const unsigned type = (x / 2 + y / 2) % 4;  // 0 black, 1 white, 2 transparent, 3 invert
      if (type >= 2) bit(b, 16, w, y, x);
      if (type == 1 || type == 3) bit(b, 16, w, y + h, x);
    } else {
      const size_t at = 16 + mask + (size_t(y) * w + x) * 4;
      const uint8_t alpha = uint8_t(x <= y ? 255 : 0);
      b[at] = uint8_t(40 * alpha / 255); b[at + 1] = uint8_t(160 * alpha / 255); b[at + 2] = uint8_t(230 * alpha / 255); b[at + 3] = alpha;
    }
  }
  return b;
}
std::vector<uint8_t> stateRecord(bool visible, bool position, uint32_t id, int32_t x = 0, int32_t y = 0) {
  std::vector<uint8_t> b(16, 0);
  b[0] = 2; b[1] = uint8_t((visible ? 1 : 0) | (position ? 2 : 0)); put32(b, 4, id); put32(b, 8, uint32_t(x)); put32(b, 12, uint32_t(y));
  return b;
}
HRESULT apply(multitap::CursorPresenter& presenter, int64_t lease, uint32_t epoch, const std::vector<uint8_t>& record) {
  return presenter.Apply(lease, epoch, record.data(), record.size());
}
SIZE handleSize(HCURSOR cursor) {
  ICONINFO info{};
  if (!cursor || !GetIconInfo(reinterpret_cast<HICON>(cursor), &info)) return SIZE{};
  BITMAP bitmap{};
  GetObjectW(info.hbmMask, sizeof(bitmap), &bitmap);
  const bool color = info.hbmColor != nullptr;
  if (info.hbmMask) DeleteObject(info.hbmMask);
  if (info.hbmColor) DeleteObject(info.hbmColor);
  return SIZE{bitmap.bmWidth, color ? bitmap.bmHeight : bitmap.bmHeight / 2};
}

void parse() {
  using multitap::ParseCursorRecord;
  const auto colour = shapeRecord(7, 1, 32, 32, 3, 4);
  const auto parsed = ParseCursorRecord(colour.data(), colour.size());
  require(parsed && parsed->isShape && parsed->shape.id == 7 && parsed->shape.shape.width == 32 && parsed->shape.shape.hotspotX == 3 &&
          parsed->shape.shape.hotspotY == 4 && parsed->shape.shape.format == multitap::RemoteCursorFormat::ColorBgraAnd &&
          parsed->shape.shape.color.size() == 32u * 32 * 4, "colour shape record parse");
  for (const unsigned width : {1u, 31u, 33u, 256u}) {
    const auto mono = shapeRecord(8, 2, width, 17, width - 1, 16);
    const auto value = ParseCursorRecord(mono.data(), mono.size());
    require(value && value->isShape && value->shape.shape.format == multitap::RemoteCursorFormat::MonochromeAndXor &&
            value->shape.shape.mask.size() == stride(width) * 34 && value->shape.shape.color.empty(), "monochrome shape record parse " + std::to_string(width));
  }
  const auto state = stateRecord(true, true, 9, 1919, 1079);
  const auto s = ParseCursorRecord(state.data(), state.size());
  require(s && !s->isShape && s->state.visible && s->state.position && s->state.id == 9 && s->state.x == 1919 && s->state.y == 1079, "state record parse");
  const auto hidden = stateRecord(false, false, 0);
  require(ParseCursorRecord(hidden.data(), hidden.size()).has_value(), "hidden state parse");
  // Malformed records never parse.
  std::vector<std::vector<uint8_t>> bad;
  auto c = colour;
  bad.push_back(std::vector<uint8_t>(colour.begin(), colour.begin() + 15));
  c = colour; c[2] = 1; bad.push_back(c);
  c = colour; c[3] = 1; bad.push_back(c);
  c = colour; c[0] = 3; bad.push_back(c);
  c = colour; c[1] = 0; bad.push_back(c);
  c = colour; c[1] = 3; bad.push_back(c);
  c = colour; put32(c, 4, 0); bad.push_back(c);
  c = colour; put32(c, 4, 0x80000000u); bad.push_back(c);
  c = colour; put16(c, 8, 0); bad.push_back(c);
  c = shapeRecord(1, 1, 32, 32); put16(c, 8, 257); bad.push_back(c);
  c = colour; put16(c, 12, 32); bad.push_back(c);
  c = colour; put16(c, 14, 32); bad.push_back(c);
  c = colour; c.pop_back(); bad.push_back(c);
  c = colour; c.push_back(0); bad.push_back(c);
  c = colour; c[1] = 2; bad.push_back(c);  // monochrome length differs
  c = shapeRecord(1, 2, 31, 8); c[16 + 3] |= 1; bad.push_back(c);  // a padding bit past width 31
  c = state; c.push_back(0); bad.push_back(c);
  c = state; c[1] = 4; bad.push_back(c);
  bad.push_back(stateRecord(true, false, 0));
  bad.push_back(stateRecord(false, false, 3));
  bad.push_back(stateRecord(true, false, 3, 1, 0));
  bad.push_back(stateRecord(true, true, 3, -1, 0));
  c = state; put32(c, 4, 0x80000000u); bad.push_back(c);
  for (const auto& record : bad) {
    require(!ParseCursorRecord(record.data(), record.size()).has_value(), "malformed record " + std::to_string(malformedCases) + " parsed");
    ++malformedCases;
  }
  require(!ParseCursorRecord(nullptr, 16).has_value(), "null record parsed");
  // A position is bounded by the announced source, not by the record: the
  // geometry limit is no longer 16384 (video_geometry.dart, server cursor.mjs
  // accept any side up to 0x7ffffffe), so the parser takes it and Apply checks
  // it against the picture's source (warps()).
  const auto beyond = stateRecord(true, true, 3, 0, 16384);
  const auto farParsed = ParseCursorRecord(beyond.data(), beyond.size());
  require(farParsed && farParsed->state.position && farParsed->state.y == 16384, "a position beyond 16383 is a well-formed record");
}

void scaling() {
  const auto record = shapeRecord(1, 2, 32, 32, 9, 13);
  const auto shape = multitap::ParseCursorRecord(record.data(), record.size())->shape.shape;
  const auto same = multitap::ScaleCursorShape(shape, 1.04);
  require(same.width == 32 && same.mask == shape.mask, "within 5 % keeps the exact shape");
  const auto half = multitap::ScaleCursorShape(shape, 0.5);
  require(half.width == 16 && half.height == 16 && half.hotspotX == 4 && half.hotspotY == 6 && multitap::validRemoteCursorShape(half), "half-size monochrome shape");
  // Nearest neighbour: target (x,y) samples source (2x+1, 2y+1); the XOR plane follows.
  bool exact = true;
  for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
    const auto source = [&](unsigned plane, unsigned sx, unsigned sy) { return (shape.mask[(sy + plane * 32) * stride(32) + sx / 8] >> (7 - sx % 8)) & 1; };
    const auto target = [&](unsigned plane) { return (half.mask[(y + plane * 16) * stride(16) + x / 8] >> (7 - x % 8)) & 1; };
    exact = exact && target(0) == source(0, 2 * x + 1, 2 * y + 1) && target(1) == source(1, 2 * x + 1, 2 * y + 1);
  }
  require(exact, "nearest-neighbour masks");
  const auto colourRecord = shapeRecord(2, 1, 48, 40, 47, 39);
  const auto colour = multitap::ParseCursorRecord(colourRecord.data(), colourRecord.size())->shape.shape;
  const auto big = multitap::ScaleCursorShape(colour, 1.5);
  require(big.width == 72 && big.height == 60 && big.hotspotX < 72 && big.hotspotY < 60 && multitap::validRemoteCursorShape(big), "upscaled colour shape");
  const auto capped = multitap::ScaleCursorShape(colour, 20.0);
  require(capped.width == 256 && capped.height <= 256 && multitap::validRemoteCursorShape(capped), "scale capped at 256");
  const auto tiny = multitap::ScaleCursorShape(colour, 0.001);
  require(tiny.width == 1 && tiny.height == 1 && multitap::validRemoteCursorShape(tiny), "scale floors at one pixel");
}

// Emulates the Flutter view's own WM_SETCURSOR handling: it sets its current
// cursor (nullptr = `none`) and halts processing.
struct Engine {
  bool enabled = false;
  HCURSOR choice = nullptr;
  unsigned calls = 0;
} engine;
LRESULT CALLBACK EngineProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_SETCURSOR && engine.enabled && LOWORD(lparam) == HTCLIENT) {
    ++engine.calls;
    FakeSet(engine.choice);
    return TRUE;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
struct Window {
  HWND value = nullptr;
  Window() {
    WNDCLASSW type{};
    type.lpfnWndProc = EngineProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapCursorPresenterTest";
    RegisterClassW(&type);  // class cursor null: DefWindowProc never sets one
    value = CreateWindowExW(0, type.lpszClassName, L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, type.hInstance, nullptr);
    require(value != nullptr, "hidden test window");
  }
  ~Window() { if (value) DestroyWindow(value); }
};
LRESULT setCursor(HWND view, WORD hit = HTCLIENT) { return SendMessageW(view, WM_SETCURSOR, WPARAM(view), MAKELPARAM(hit, WM_MOUSEMOVE)); }

void presentation() {
  Window window;
  const HWND view = window.value;
  bool captured = false;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  require(presenter.Attach(view), "attach subclass");
  fake = Fake{}; fake.under = view; fake.pos = POINT{50, 60};
  const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW), hand = LoadCursorW(nullptr, IDC_HAND);
  require(apply(presenter, 5, 0, shapeRecord(1, 1, 32, 32, 0, 0)) == S_OK && presenter.cachedShapes() == 1 && presenter.lease() == 5, "first shape cached");
  require(apply(presenter, 5, 0, stateRecord(true, false, 1)) == S_OK && presenter.visible() && presenter.shapeId() == 1, "visible state");
  // Not captured: Flutter's `none` stays, nothing is set and no timer runs.
  fake.current = nullptr; fake.sets = 0;
  require(!presenter.Present() && fake.current == nullptr && fake.sets == 0 && !presenter.timerRunning(), "uncaptured view keeps Flutter's cursor");
  captured = true;
  presenter.Refresh();
  require(presenter.timerRunning() && presenter.Owns(fake.current) && handleSize(fake.current).cx == 32, "captured view shows the remote shape");
  const HCURSOR shown = fake.current;
  // Flutter chose a real cursor (toolbar): untouched.
  fake.current = hand;
  require(!presenter.Present() && fake.current == hand, "Flutter cursor outside the picture is untouched");
  setCursor(view);
  require(fake.current == hand, "WM_SETCURSOR keeps a Flutter cursor");
  // Flutter's `none` over the picture is replaced by the remote shape.
  fake.current = nullptr;
  require(setCursor(view) == TRUE && fake.current == shown, "WM_SETCURSOR replaces `none` with the remote shape");
  fake.current = nullptr;
  setCursor(view, HTCAPTION);
  require(fake.current == nullptr, "non-client hit test untouched");
  // Pointer over another window: untouched.
  fake.under = nullptr; fake.current = nullptr;
  require(!presenter.Present() && fake.current == nullptr, "pointer outside the view untouched");
  fake.under = view;
  // Hidden remote cursor hides the local one over the picture.
  fake.current = shown;
  require(apply(presenter, 5, 0, stateRecord(false, false, 0)) == S_OK && fake.current == nullptr && !presenter.timerRunning(), "hidden remote cursor hides the presenter cursor");
  require(setCursor(view) == TRUE && fake.current == nullptr, "hidden stays hidden on WM_SETCURSOR");
  // A second shape, then switching between cached ids.
  require(apply(presenter, 5, 0, shapeRecord(2, 2, 32, 32, 15, 15)) == S_OK && apply(presenter, 5, 0, stateRecord(true, false, 2)) == S_OK && presenter.Owns(fake.current) && fake.current != shown, "second shape shown");
  require(apply(presenter, 5, 0, stateRecord(true, false, 1)) == S_OK && fake.current == shown, "cached first shape reused");
  // Sequence rules.
  require(apply(presenter, 5, 0, shapeRecord(2, 1, 8, 8)) == E_INVALIDARG && presenter.cachedShapes() == 2, "non-increasing shape id rejected");
  require(apply(presenter, 5, 0, stateRecord(true, false, 3)) == E_INVALIDARG && presenter.shapeId() == 1, "state naming a never-sent id rejected");
  require(apply(presenter, 5, 2, shapeRecord(3, 1, 8, 8)) == S_OK && apply(presenter, 5, 1, stateRecord(true, false, 3)) == S_FALSE && presenter.shapeId() == 1, "stale epoch ignored");
  // Capture release: the next WM_SETCURSOR/timer leaves Flutter in charge.
  captured = false;
  fake.current = nullptr;
  setCursor(view);
  require(fake.current == nullptr, "released capture does not present");
  SendMessageW(view, WM_TIMER, multitap::kCursorTimer, 0);
  require(!presenter.timerRunning(), "timer stops itself after capture release");
  captured = true;
  // Another thread cannot drive the presenter.
  HRESULT foreign = S_OK;
  std::thread other([&] { const auto record = stateRecord(true, false, 1); foreign = presenter.Apply(5, 2, record.data(), record.size()); });
  other.join();
  require(foreign == E_INVALIDARG, "foreign thread rejected");
  // Reset: the arrow replaces a presenter cursor that is still current.
  presenter.Refresh();
  require(presenter.Owns(fake.current), "presenting before reset");
  presenter.Reset();
  // The presenter's own copy of the arrow, never the shared stock handle: a
  // stock arrow could not be told from one Flutter chose (cursor_presenter.h
  // "fallback_"), so Present() could not replace it on re-entry.
  require(fake.current != nullptr && fake.current != arrow && presenter.Owns(fake.current) && handleSize(fake.current).cx == handleSize(arrow).cx &&
          presenter.cachedShapes() == 0 && presenter.lease() == 0 && !presenter.timerRunning() && !presenter.visible(), "reset restores the arrow");
  // A new lease starts clean; ids may restart.
  require(apply(presenter, 6, 0, shapeRecord(1, 1, 16, 16)) == S_OK && presenter.lease() == 6 && presenter.cachedShapes() == 1, "new lease accepted");
  require(apply(presenter, 7, 0, stateRecord(true, false, 1)) == E_INVALIDARG && presenter.lease() == 7 && presenter.cachedShapes() == 0, "new lease resets old shapes");
  // Detach: the subclass is gone.
  fake.current = nullptr;  // Flutter's `none` over the captured picture again
  require(apply(presenter, 7, 0, shapeRecord(1, 1, 16, 16)) == S_OK && apply(presenter, 7, 0, stateRecord(true, false, 1)) == S_OK && presenter.Owns(fake.current), "presenting before detach");
  presenter.Detach();
  require(fake.current == arrow && presenter.view() == nullptr, "detach restores the arrow");
  fake.current = nullptr;
  setCursor(view);
  require(fake.current == nullptr, "detached window no longer overridden");
}

// The Flutter view answers WM_SETCURSOR first; the presenter keeps its shape
// without a hide/show pair while that answer is fresh and still `none`.
void engineInterplay() {
  Window window;
  const HWND view = window.value;
  bool captured = true;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  presenter.Attach(view);
  fake = Fake{}; fake.under = view;
  engine = Engine{}; engine.enabled = true;
  const HCURSOR hand = LoadCursorW(nullptr, IDC_HAND);
  require(apply(presenter, 9, 0, shapeRecord(1, 1, 32, 32)) == S_OK && apply(presenter, 9, 0, stateRecord(true, false, 1)) == S_OK, "engine fixture state");
  const HCURSOR shown = fake.current;
  require(presenter.Owns(shown), "presented before the first WM_SETCURSOR");
  require(setCursor(view) == TRUE && engine.calls == 1 && fake.current == shown, "Flutter consulted first, `none` replaced");
  const unsigned sets = fake.sets;
  for (int n = 0; n < 20; ++n) require(setCursor(view) == TRUE, "fresh `none` kept");
  require(engine.calls == 1 && fake.sets == sets && fake.current == shown, "20 pointer moves: no Flutter hide, no SetCursor at all");
  fake.ticks += multitap::kCursorFlutterRecheckMs;
  require(setCursor(view) == TRUE && engine.calls == 2 && fake.current == shown, "Flutter re-read after the recheck interval");
  // Flutter switched to the toolbar cursor and applied it at once.
  engine.choice = hand; FakeSet(hand);
  setCursor(view);
  require(engine.calls == 3 && fake.current == hand, "toolbar cursor detected immediately and kept");
  setCursor(view);
  require(engine.calls == 4 && fake.current == hand, "foreign cursor: Flutter consulted every time");
  // Back over the picture.
  engine.choice = nullptr; FakeSet(nullptr);
  require(setCursor(view) == TRUE && engine.calls == 5 && fake.current == shown, "back over the picture");
  // A Flutter that only stored its new choice: at most one recheck interval late.
  engine.choice = hand;
  setCursor(view);
  require(engine.calls == 5 && fake.current == shown, "stored-only change not seen within the interval");
  fake.ticks += multitap::kCursorFlutterRecheckMs;
  setCursor(view);
  require(engine.calls == 6 && fake.current == hand, "stored-only change applied within one recheck interval");
  // Not captured: Flutter decides on every message.
  engine.choice = nullptr; FakeSet(nullptr); setCursor(view);
  captured = false;
  const unsigned before = engine.calls;
  setCursor(view);
  require(engine.calls == before + 1 && fake.current == nullptr, "uncaptured: Flutter only");
  engine = Engine{};
}

void cache() {
  Window window;
  bool captured = true;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  presenter.Attach(window.value);
  fake = Fake{}; fake.under = window.value;
  require(apply(presenter, 1, 0, shapeRecord(1, 1, 8, 8)) == S_OK && apply(presenter, 1, 0, stateRecord(true, false, 1)) == S_OK, "current shape");
  for (uint32_t id = 2; id <= 80; ++id) require(apply(presenter, 1, 0, shapeRecord(id, id % 2 ? 1 : 2, 8 + id % 5, 8)) == S_OK, "cache insert");
  require(presenter.cachedShapes() == multitap::kCursorCacheLimit && presenter.Cached(1) && presenter.Cached(80) && !presenter.Cached(2), "LRU keeps 64, the current shape and the newest");
  require(presenter.Owns(fake.current) && apply(presenter, 1, 0, stateRecord(true, false, 80)) == S_OK && presenter.Owns(fake.current), "switch to newest");
  // The fallback is the presenter's own arrow copy (see presentation()).
  require(FAILED(apply(presenter, 1, 0, stateRecord(true, false, 2))) && presenter.visible() && !presenter.Cached(2) &&
              fake.current != nullptr && fake.current != LoadCursorW(nullptr, IDC_ARROW) && presenter.Owns(fake.current) && !presenter.timerRunning(),
          "an evicted id reports unavailable and uses a local fallback");
}

void scaledPresentation() {
  Window window;
  bool captured = true;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  presenter.Attach(window.value);
  fake = Fake{}; fake.under = window.value; fake.dpi = 144;
  // 1280 logical * 1.5 = 1920 physical for a 3840 source: half size.
  require(presenter.SetPicture(3, multitap::CursorPicture{0, 0, 1280, 720, 3840, 2160}) == S_OK, "picture");
  require(apply(presenter, 3, 0, shapeRecord(1, 1, 32, 32, 31, 31)) == S_OK && apply(presenter, 3, 0, stateRecord(true, false, 1)) == S_OK, "scaled state");
  require(handleSize(fake.current).cx == 16, "picture/source scale applied (0.5)");
  const HCURSOR small = fake.current;
  require(presenter.SetPicture(3, multitap::CursorPicture{0, 0, 2560, 1440, 3840, 2160}) == S_OK && handleSize(fake.current).cx == 32 && fake.current != small, "rescaled to 1:1 with the current cursor swapped first");
  require(presenter.SetPicture(3, multitap::CursorPicture{0, 0, 0, 1440, 3840, 2160}) == E_INVALIDARG, "invalid picture rejected");
  require(presenter.SetPicture(0, multitap::CursorPicture{0, 0, 10, 10, 10, 10}) == E_INVALIDARG, "zero lease rejected");
}

void warps() {
  Window window;
  bool captured = true;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  presenter.Attach(window.value);
  fake = Fake{}; fake.under = window.value; fake.origin = POINT{100, 200};
  require(presenter.SetPicture(4, multitap::CursorPicture{10, 20, 960, 540, 1920, 1080}) == S_OK, "warp picture");
  require(apply(presenter, 4, 0, shapeRecord(1, 1, 32, 32)) == S_OK, "warp shape");
  fake.pos = POINT{100 + 10 + 5, 200 + 20 + 5};
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 1919, 1079)) == S_OK && fake.moves.size() == 1, "warp applied");
  require(fake.moves[0].x == 100 + 10 + 960 - 1 && fake.moves[0].y == 200 + 20 + 540 - 1, "warp maps x/(sourceWidth-1) into the picture (clamped inside)");
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 0, 0)) == S_OK && fake.moves.size() == 2 && fake.moves[1].x == 110 && fake.moves[1].y == 220, "warp to the picture origin");
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 960, 540)) == S_OK && fake.moves.size() == 3 && fake.moves[2].x == 110 + 480 && fake.moves[2].y == 220 + 270, "warp to the centre");
  // The move our SetCursorPos produced is not user movement.
  SendMessageW(window.value, WM_MOUSEMOVE, 0, MAKELPARAM(10 + 480, 20 + 270));
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 100, 100)) == S_OK && fake.moves.size() == 4, "own warp move ignored");
  // A real local move blocks warps for 50 ms.
  SendMessageW(window.value, WM_MOUSEMOVE, 0, MAKELPARAM(300, 300));
  fake.ticks += 20;
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 200, 200)) == S_OK && fake.moves.size() == 4, "local movement blocks a warp");
  fake.ticks += 40;
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 200, 200)) == S_OK && fake.moves.size() == 5, "warp after 50 ms stillness");
  // Not captured / outside the picture / over another window: no warp.
  captured = false;
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 300, 300)) == S_OK && fake.moves.size() == 5, "no warp without capture");
  captured = true;
  fake.pos = POINT{100 + 2, 200 + 2};
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 300, 300)) == S_OK && fake.moves.size() == 5, "no warp from the letterbox");
  fake.pos = POINT{100 + 10 + 5, 200 + 20 + 5}; fake.under = nullptr;
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 300, 300)) == S_OK && fake.moves.size() == 5, "no warp over another window");
  fake.under = window.value;
  require(apply(presenter, 4, 0, stateRecord(true, true, 1, 1920, 0)) == E_INVALIDARG && fake.moves.size() == 5, "position outside the source rejected");
  require(presenter.warps() == 5, "warp count");
}

// relative-mouse-v1: capture ended, with or without keyboard capture.
void remoteReturn() {
  Window window;
  bool captured = false;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  presenter.Attach(window.value);
  fake = Fake{}; fake.under = window.value; fake.origin = POINT{100, 200};
  require(presenter.SetPicture(4, multitap::CursorPicture{10, 20, 960, 540, 1920, 1080}) == S_OK, "return picture");
  fake.pos = POINT{100 + 10 + 5, 200 + 20 + 5};
  presenter.MoveToRemote();
  require(fake.moves.empty(), "an unknown remote position moved the pointer");
  require(apply(presenter, 4, 0, stateRecord(false, false, 0)) == S_OK, "hidden state");
  presenter.MoveToRemote();
  require(fake.moves.empty(), "a state without a position moved the pointer");
  // Reported while parked; the local move a moment ago does not block the return.
  require(apply(presenter, 4, 0, stateRecord(false, true, 0, 960, 540)) == S_OK && fake.moves.empty(), "a warp ran without capture");
  SendMessageW(window.value, WM_MOUSEMOVE, 0, MAKELPARAM(300, 300));
  presenter.MoveToRemote();
  require(fake.moves.size() == 1 && fake.moves[0].x == 110 + 480 && fake.moves[0].y == 220 + 270, "the pointer did not return to the remote position");
  fake.under = nullptr;
  presenter.MoveToRemote();
  require(fake.moves.size() == 1, "the pointer moved over another window");
  fake.under = window.value;
  presenter.Reset();
  presenter.MoveToRemote();
  require(fake.moves.size() == 1, "a retired session's position moved the pointer");
}

// stream-size-v1: the Host restarts its encoder inside one capture epoch (a new
// stream.info, then an IDR). The client starts a new decoder under a new media
// lease (remote_session.dart _rescale -> restart), the runner resets this
// presenter for it (flutter_window.cpp "keepCapture") and the session drops
// cursor records until that IDR. The Host's real channel feeds the real
// presenter here: a shape sent before the restart (the resize arrows of a
// window edge) must still be shown after it. If the channel kept
// naming the ids the client had just forgotten, every such state would be rejected
// and the view would fall back to the local arrow for the rest of the epoch
// (the edge cursor "works, then does not").
void decoderRestartWithinEpoch() {
  namespace cursor = host::cursor;
  const auto colour = [](uint8_t seed) {
    cursor::Shape shape; shape.format = cursor::Format::colour; shape.width = shape.height = 32; shape.hotspotX = shape.hotspotY = seed;
    shape.mask.assign(cursor::maskStride(32) * 32, 0); shape.colour.assign(size_t(32) * 32 * 4, seed);
    return shape;
  };
  const HCURSOR arrow = reinterpret_cast<HCURSOR>(uintptr_t(0x11)), resize = reinterpret_cast<HCURSOR>(uintptr_t(0x22));
  cursor::SessionShapeIds ids;
  cursor::Channel channel(RECT{0, 0, 1920, 1080}, [&] { return ids.next("restart"); },
                          [&](HCURSOR handle) -> std::optional<cursor::Shape> { return colour(handle == arrow ? 3 : 9); });
  Window window;
  bool captured = true;
  multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
  require(presenter.Attach(window.value), "attach subclass");
  fake = Fake{}; fake.under = window.value; fake.pos = POINT{50, 60};
  uint64_t now = 1000000; int64_t lease = 7; const cursor::InjectedPointer::Mark none;
  // One capture tick (30 ms: past the state coalescing) and its records, applied as the session hands them on.
  const auto tick = [&](HCURSOR handle, const char* reason) {
    now += 30000;
    const auto records = channel.observe({true, CURSOR_SHOWING, handle, POINT{400, 300}}, none, now);
    for (const auto& record : records) require(apply(presenter, lease, 0, record) == S_OK, reason);
    return records.size();
  };
  channel.keyFrame();
  require(tick(arrow, "first shape and state") == 2 && presenter.visible() && presenter.Owns(fake.current), "the arrow is shown");
  const HCURSOR arrowShown = fake.current;
  require(tick(resize, "edge shape and state") == 2 && presenter.Owns(fake.current) && fake.current != arrowShown, "the edge cursor is shown");
  require(tick(arrow, "back to the arrow") == 1 && fake.current == arrowShown, "the arrow returns by its id");
  // The encoder restarts at another scale; the client's new decoder has a new lease.
  channel.pictureRestart();
  presenter.Reset(); ++lease;
  // Before the new decoder's first key frame the session drops cursor records
  // (remote_session.dart "_awaitingKeyFrame"); they are not applied here either.
  now += 30000;
  const size_t dropped = channel.observe({true, CURSOR_SHOWING, resize, POINT{400, 300}}, none, now).size();
  channel.keyFrame();
  // The pointer rests on the window edge: the resize cursor must appear.
  require(tick(resize, "the edge cursor was rejected after a decoder restart inside the epoch") >= 1, "the edge cursor was not announced to the restarted decoder");
  require(presenter.visible() && presenter.Owns(fake.current) && presenter.cachedShapes() == 1, "the edge cursor is not shown after a decoder restart inside the epoch");
  const HCURSOR resizeShown = fake.current;
  require(tick(arrow, "the arrow was rejected after a decoder restart inside the epoch") >= 1 && presenter.Owns(fake.current) && fake.current != resizeShown, "the arrow is not shown after a decoder restart inside the epoch");
  require(tick(resize, "the edge cursor's id was rejected") == 1 && fake.current == resizeShown, "the edge cursor does not return by its id after the restart");
  require(dropped == 0, "a record the client drops was sent before the restarted decoder's key frame");
  presenter.Detach();
}

void lifetime() {
  const auto gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  {
    Window window;
    bool captured = true;
    multitap::CursorPresenter presenter([&] { return captured; }, fakeOs());
    presenter.Attach(window.value);
    for (int64_t lease = 1; lease <= 150; ++lease) {
      fake = Fake{}; fake.under = window.value;
      require(presenter.SetPicture(lease, multitap::CursorPicture{0, 0, 640, 360, 640, 360}) == S_OK, "cycle picture");
      for (uint32_t id = 1; id <= 3; ++id) require(apply(presenter, lease, 0, shapeRecord(id, id == 2 ? 2 : 1, 24 + id, 24)) == S_OK, "cycle shape");
      require(apply(presenter, lease, 0, stateRecord(true, false, 3)) == S_OK && presenter.Owns(fake.current), "cycle present");
      require(presenter.SetPicture(lease, multitap::CursorPicture{0, 0, 320, 180, 640, 360}) == S_OK && presenter.Owns(fake.current), "cycle rescale");
      if (lease % 2) presenter.Reset();
    }
    presenter.Detach();
  }
  require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdi && GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == user, "GDI/USER handles leaked");
}
}  // namespace

int main() {
  // Every case runs and reports by name: one failing case must not keep the
  // others from running (the process still fails).
  const struct { const char* name; void (*run)(); } cases[] = {{"parse", parse}, {"scaling", scaling}, {"presentation", presentation},
    {"engine-interplay", engineInterplay}, {"cache", cache}, {"scaled-presentation", scaledPresentation}, {"warps", warps},
    {"remote-return", remoteReturn}, {"decoder-restart-within-epoch", decoderRestartWithinEpoch}, {"lifetime", lifetime}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) {
    try { value.run(); ++passed; }
    catch (const std::exception& error) { ++failed; std::cerr << "FAILED " << value.name << ": " << error.what() << " (after " << checks << " checks)\n"; }
  }
  std::cout << "{\"kind\":\"client-cursor-presenter\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks
            << ",\"malformedRecords\":" << malformedCases << ",\"realDesktopCursorTouched\":false}\n";
  return failed ? 1 : 0;
}
