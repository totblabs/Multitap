// capture-dirty-v1 (capture_dirty.hpp): the pure rules of the dirty-rectangle
// readback. Rectangle widening, clipping and merging; every full-copy rule;
// and, with synthetic frames, that copying and converting only the planned
// rectangles of a frame that changed inside the reported rectangles gives
// byte for byte the full copy and the full conversion. No capture, no device.
#include "capture_dirty.hpp"
#include <cstring>
#include <iostream>
#include <random>

namespace {
using namespace host::capture;
int checks = 0; std::string failures;
void require(bool ok, const std::string& what) { ++checks; if (!ok) { failures += what + "\n"; std::cerr << "FAIL: " << what << "\n"; } }
bool contains(const PixelRect& outer, const PixelRect& inner) { return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right && outer.bottom >= inner.bottom; }

void widening() {
  require(evenClipped({3, 5, 8, 9}, 100, 100) == PixelRect{2, 4, 8, 10}, "odd edges widen outward to even");
  require(evenClipped({0, 0, 1, 1}, 100, 100) == PixelRect{0, 0, 2, 2}, "a single pixel covers its 2x2 block");
  require(evenClipped({-5, -5, 3, 3}, 100, 100) == PixelRect{0, 0, 4, 4}, "clipped at the origin");
  require(evenClipped({95, 97, 200, 200}, 100, 100) == PixelRect{94, 96, 100, 100}, "clipped at the far edge");
  require(!evenClipped({100, 0, 120, 10}, 100, 100), "a rectangle outside the surface is dropped");
  require(!evenClipped({10, 10, 10, 20}, 100, 100), "an empty rectangle is dropped");
  require(!evenClipped({10, 10, 20, 20}, 0, 100), "an empty surface takes nothing");
}
void merging() {
  { const PixelRect in[] = {{0, 0, 10, 10}, {10, 0, 20, 10}}; const auto out = mergeDirtyRects(in, 100, 100); require(out.size() == 1 && out[0] == PixelRect{0, 0, 20, 10}, "edge-sharing rectangles merge"); }
  { const PixelRect in[] = {{0, 0, 10, 10}, {12, 0, 20, 10}}; const auto out = mergeDirtyRects(in, 100, 100); require(out.size() == 2, "separated rectangles stay apart"); }
  { const PixelRect in[] = {{0, 0, 10, 10}, {20, 20, 30, 30}, {10, 10, 20, 20}}; const auto out = mergeDirtyRects(in, 100, 100); require(out.size() == 1 && out[0] == PixelRect{0, 0, 30, 30}, "a chain merges through its middle rectangle"); }
  { const PixelRect in[] = {{1, 1, 3, 3}, {4, 4, 6, 6}}; const auto out = mergeDirtyRects(in, 100, 100); require(out.size() == 1 && out[0] == PixelRect{0, 0, 6, 6}, "rectangles that touch after widening merge"); }
  { const PixelRect in[] = {{200, 200, 300, 300}, {5, 5, 7, 7}}; const auto out = mergeDirtyRects(in, 100, 100); require(out.size() == 1 && out[0] == PixelRect{4, 4, 8, 8}, "outside rectangles are dropped"); }
  require(mergeDirtyRects({}, 100, 100).empty(), "no rectangles: nothing");
  // Property: outputs never touch each other and cover every widened input.
  std::mt19937 random(5);
  for (int round = 0; round < 300; ++round) {
    std::vector<PixelRect> in; const int count = 1 + int(random() % 12);
    for (int i = 0; i < count; ++i) { const int x = int(random() % 140) - 20, y = int(random() % 140) - 20; in.push_back({x, y, x + 1 + int(random() % 50), y + 1 + int(random() % 50)}); }
    const auto out = mergeDirtyRects(in, 120, 120);
    bool separate = true, covered = true, even = true;
    for (size_t i = 0; i < out.size(); ++i) { even = even && evenRect(out[i]) && out[i].left >= 0 && out[i].top >= 0 && out[i].right <= 120 && out[i].bottom <= 120; for (size_t j = i + 1; j < out.size(); ++j) separate = separate && !touches(out[i], out[j]); }
    for (const auto& r : in) if (const auto widened = evenClipped(r, 120, 120)) { bool inside = false; for (const auto& o : out) inside = inside || contains(o, *widened); covered = covered && inside; }
    require(separate && covered && even, "merged rectangles are even, inside, pairwise apart and cover every input");
  }
}
void rules() {
  const PixelRect one[] = {{10, 10, 30, 30}};
  DirtyPolicy policy;
  require(planDirtyCopy({true, true, one, 200, 200, 0, 0, policy}).full == false, "a known small change is a partial copy");
  require(planDirtyCopy({false, true, one, 200, 200, 0, 0, policy}).full, "an incomplete staging texture: full copy");
  require(planDirtyCopy({true, false, one, 200, 200, 0, 0, policy}).full, "unknown rectangles: full copy");
  require(!planDirtyCopy({true, true, one, 200, 200, 1000000000, 0, policy}).full, "no periodic full copy by default (the sweep band refreshes)");
  { DirtyPolicy periodic; periodic.fullIntervalUs = 1000000;
    require(planDirtyCopy({true, true, one, 200, 200, periodic.fullIntervalUs, 0, periodic}).full, "the periodic full copy is due");
    require(!planDirtyCopy({true, true, one, 200, 200, periodic.fullIntervalUs - 1, 0, periodic}).full, "just before the period: still partial"); }
  { const auto plan = planDirtyCopy({true, true, {}, 200, 200, 0, 0, policy}); require(!plan.full && plan.rects.empty() && plan.sweep.has_value(), "nothing changed: only the sweep band"); }
  { DirtyPolicy none; none.sweepBands = 0; require(!planDirtyCopy({true, true, one, 200, 200, 0, 0, none}).sweep.has_value(), "no bands: no sweep"); }
  require(!planDirtyCopy({false, true, one, 200, 200, 0, 0, policy}).sweep.has_value(), "a full plan has no sweep band");
  { const PixelRect big[] = {{0, 0, 200, 122}}; require(planDirtyCopy({true, true, big, 200, 200, 0, 0, policy}).full, "61% of the surface: full copy"); }
  { const PixelRect big[] = {{0, 0, 200, 118}}; require(!planDirtyCopy({true, true, big, 200, 200, 0, 0, policy}).full, "59% of the surface: partial"); }
  { std::vector<PixelRect> many; for (int i = 0; i < 25; ++i) many.push_back({i * 8, 0, i * 8 + 2, 2}); require(planDirtyCopy({true, true, many, 400, 400, 0, 0, policy}).full, "25 separate rectangles: full copy"); many.pop_back(); require(!planDirtyCopy({true, true, many, 400, 400, 0, 0, policy}).full, "24 separate rectangles: partial"); }
  { std::vector<PixelRect> many(policy.maxInputRects + 1, PixelRect{0, 0, 2, 2}); require(planDirtyCopy({true, true, many, 400, 400, 0, 0, policy}).full, "too many reported rectangles: full copy without merging"); }
  require(planDirtyCopy({true, true, one, 0, 0, 0, 0, policy}).full, "an empty surface: full copy");
  { DirtyPolicy strict; strict.fullAreaPercent = 0; require(planDirtyCopy({true, true, one, 200, 200, 0, 0, strict}).full, "zero percent allows no partial copy"); }
}
void sweep() {
  // 64 bands of 1340 rows: 22 even rows each, 61 bands, the last one shorter; consecutive indices cover the surface exactly once.
  std::vector<int> covered(1340, 0); int count = 0;
  for (int index = 0; index < 61; ++index) { const auto band = sweepBand(index, 3424, 1340, 64); require(band && band->left == 0 && band->right == 3424 && evenRect(*band) == (band->bottom % 2 == 0), "a band spans the width at even rows"); for (int y = band->top; y < band->bottom; ++y) ++covered[size_t(y)]; ++count; }
  require(std::all_of(covered.begin(), covered.end(), [](int n) { return n == 1; }), "61 consecutive bands cover 1340 rows exactly once");
  require(sweepBand(61, 3424, 1340, 64) == sweepBand(0, 3424, 1340, 64), "the index wraps");
  require(sweepBand(-1, 3424, 1340, 64) == sweepBand(60, 3424, 1340, 64), "a negative index wraps too");
  require(sweepBand(0, 64, 64, 64) == PixelRect{0, 0, 64, 2} && sweepBand(31, 64, 64, 64) == PixelRect{0, 62, 64, 64}, "a tiny surface: two-row bands");
  require(sweepBand(5, 100, 100, 1) == PixelRect{0, 0, 100, 100}, "one band is the whole surface");
  require(!sweepBand(0, 100, 100, 0) && !sweepBand(0, 0, 100, 8), "no bands or no surface: none");
}
// Synthetic frames: B differs from A only inside the reported rectangles (any
// alignment). Copying only the planned rectangles of B over a copy of A gives B;
// converting only them into A's NV12 gives B's full conversion.
void exactness() {
  const int w = 96, h = 64; std::mt19937 random(11);
  std::vector<uint8_t> a(size_t(w) * h * 4);
  for (auto& byte : a) byte = uint8_t(random());
  for (int round = 0; round < 200; ++round) {
    std::vector<uint8_t> b = a; std::vector<PixelRect> reported; const int count = int(random() % 6);
    for (int i = 0; i < count; ++i) {
      const int x = int(random() % 90), y = int(random() % 60); const PixelRect r{x, y, std::min(w, x + 1 + int(random() % 30)), std::min(h, y + 1 + int(random() % 20))};
      reported.push_back(r);
      for (int yy = r.top; yy < r.bottom; ++yy) for (int xx = r.left; xx < r.right; ++xx) if (random() % 3) { auto* p = b.data() + (size_t(yy) * w + xx) * 4; p[0] = uint8_t(random()); p[1] = uint8_t(random()); p[2] = uint8_t(random()); }
    }
    DirtyPolicy policy; policy.fullAreaPercent = 100; policy.maxRects = 64;
    const auto plan = planDirtyCopy({true, true, reported, w, h, 0, round, policy});
    require(!plan.full && plan.sweep, "exactness: a partial plan with a sweep band");
    std::vector<PixelRect> pieces = plan.rects; pieces.push_back(*plan.sweep);
    std::vector<uint8_t> staging = a;
    for (const auto& r : pieces) for (int y = r.top; y < r.bottom; ++y) std::memcpy(staging.data() + (size_t(y) * w + r.left) * 4, b.data() + (size_t(y) * w + r.left) * 4, size_t(r.width()) * 4);
    require(staging == b, "copying the planned rectangles gives the new frame");
    std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2), expected(nv12.size());
    convertBlockToNv12(nv12, w, h, a.data(), size_t(w) * 4, {0, 0, w, h});
    for (const auto& r : pieces) convertBlockToNv12(nv12, w, h, b.data() + (size_t(r.top) * w + r.left) * 4, size_t(w) * 4, r);
    convertBlockToNv12(expected, w, h, b.data(), size_t(w) * 4, {0, 0, w, h});
    require(nv12 == expected, "converting the planned rectangles gives the full conversion");
    a = b;
  }
}
}

int main() {
  try { widening(); merging(); rules(); sweep(); exactness(); }
  catch (const std::exception& error) { std::cerr << "ERROR: " << error.what() << "\n"; return 2; }
  if (!failures.empty()) { std::cerr << failures; return 1; }
  std::cout << "capture dirty test passed (" << checks << " checks)\n";
  return 0;
}
