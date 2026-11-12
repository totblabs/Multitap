// The picture rectangle the mouse lock clips to (pointer_confinement.h), pure
// arithmetic: no window, no clip, no input. There is no separate
// mouse-confine mode and no Ctrl+Alt chord.
#include "pointer_confinement.h"

#include <climits>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
  ++checks;
}
bool same(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }

using multitap::ConfinementRect;
using multitap::LogicalRect;

void rectangles() {
  const POINT origin{100, 50};
  const SIZE client{1600, 1000};
  // Every edge keeps a 2-logical-pixel margin (3 px at 125%/150%) so the
  // cursor never touches the window border or caption.
  auto r = ConfinementRect({0, 0, 1280, 800}, 120, origin, client);
  require(r && same(*r, RECT{103, 53, 1697, 1047}), "125% picture did not map to its physical screen pixels");
  r = ConfinementRect({0, 0, 1280, 800}, 96, origin, client);
  require(r && same(*r, RECT{102, 52, 1378, 848}), "100% picture changed size");
  // Inward rounding: the hot spot never reaches the letterbox beside it.
  r = ConfinementRect({10.3, 20.6, 100.2, 50.1}, 96, POINT{0, 0}, client);
  require(r && same(*r, RECT{13, 23, 108, 68}), "Fractional edges were not rounded inward");
  // 1279.9999 is the picture edge, not one pixel short of it.
  r = ConfinementRect({0, 0, 1023.9999999, 639.99999999}, 120, POINT{0, 0}, SIZE{1280, 800});
  require(r && same(*r, RECT{3, 3, 1277, 797}), "Float noise lost an edge pixel");
  // Never past the client area (stale layout during a resize).
  r = ConfinementRect({-40, -40, 4000, 4000}, 144, POINT{-1920, 0}, SIZE{1920, 1080});
  require(r && same(*r, RECT{-1917, 3, -3, 1077}), "Rectangle escaped the client area on a left monitor");
  // A rectangle too small for the margin keeps its own size.
  r = ConfinementRect({0, 0, 6, 6}, 96, POINT{0, 0}, client);
  require(r && same(*r, RECT{0, 0, 6, 6}), "A tiny picture lost its whole area to the margin");
  require(ConfinementRect({0, 0, 800, 600}, 0, POINT{0, 0}, client).has_value(), "Unknown DPI did not fall back to 96");
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  for (const LogicalRect bad : {LogicalRect{nan, 0, 10, 10}, LogicalRect{0, 0, inf, 10}, LogicalRect{0, 0, 0, 10},
                                LogicalRect{0, 0, 10, -1}, LogicalRect{2000, 0, 10, 10}, LogicalRect{0, 0, 0.4, 0.4},
                                LogicalRect{1e9, 0, 10, 10}}) {
    require(!ConfinementRect(bad, 96, POINT{0, 0}, client), "Invalid or empty rectangle was accepted");
  }
  require(!ConfinementRect({0, 0, 10, 10}, 96, POINT{0, 0}, SIZE{0, 0}), "Empty client area was accepted");
  require(!ConfinementRect({0, 0, 10, 10}, 9600, POINT{0, 0}, client), "Absurd DPI was accepted");
  require(!ConfinementRect({0, 0, 10, 10}, 96, POINT{LONG_MAX - 4, 0}, client), "Coordinate overflow was accepted");
}
}  // namespace

int main() {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT before{};
    GetClipCursor(&before);
    rectangles();
    RECT after{};
    GetClipCursor(&after);
    require(EqualRect(&before, &after) != FALSE, "The real cursor clip changed during QA");
    std::cout << "{\"passed\":true,\"checks\":" << checks
              << ",\"realClipCursorCalled\":false,\"actualHiddenWindow\":false,\"windowShown\":false,"
                 "\"physicalInput\":false,\"realSessionVerified\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
