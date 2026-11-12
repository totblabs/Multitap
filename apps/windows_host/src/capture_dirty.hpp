#pragma once
#include "capture_compose.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

// capture-dirty-v1: read back only what changed on the CPU capture path.
//
// Windows Graphics Capture (Windows 11 24H2, build 26100) reports with every
// delivered frame the rectangles that changed since the previous delivered
// frame (GraphicsCaptureSession.DirtyRegionMode = ReportOnly,
// Direct3D11CaptureFrame.DirtyRegions). The CPU path keeps, per captured
// monitor, a persistent staging texture holding the complete current frame and
// a complete NV12 picture. With dirty rectangles a tick copies only those from
// the pool texture into the staging texture (CopySubresourceRegion), maps it
// and converts only them; the rest of the staging and the NV12 are still
// current from earlier ticks. Nothing is approximated: the staging texture and
// the NV12 are byte for byte what a full copy and a full conversion would give
// (MultitapHostCaptureReadbackBenchmark compares both every frame, also after
// frames the pool dropped while the loop stalled).
//
// Why: when the display is composed by the Basic Render Driver (WARP; seen on a
// VM after a Host update) the whole-frame CopyResource -> Map of a
// 2908x1340 frame took 45-90 ms per frame (stream stats captureUs), 10-19 fps,
// while the same desktop on the NVIDIA device reads back in about 2 ms. WARP
// copies with the CPU, so the cost is proportional to the bytes copied; a
// typing or pointer change dirties a few thousand pixels of four million.
//
// Rules (planDirtyCopy), each one a full copy:
//  - the staging texture does not hold a complete frame yet: first frame,
//    recreated texture, or GPU ticks (gpu_capture.hpp) consumed frames without
//    a readback since;
//  - the dirty rectangles of any frame taken this tick are unknown (an OS
//    without the API, the session did not confirm ReportOnly, a frame without
//    the interface, reading them failed);
//  - the merged rectangles cover more than fullAreaPercent of the surface or
//    more than maxRects remain after merging (one large copy beats many small
//    ones; a scroll or a video dirties most of the frame anyway);
//  - fullIntervalUs (when not 0) passed since the last full copy.
// Otherwise the plan lists even, clipped, merged rectangles, plus one sweep
// band: a horizontal 1/sweepBands slice of the surface, the next one each
// partial tick, so every pixel is re-read from a real frame within sweepBands
// partial ticks (about a second at 60 fps) without a whole-frame copy ever
// stalling a tick. The band is the bound on any divergence should the OS
// under-report a change (none seen on WARP in the benchmark; insurance), and
// it is exact: it copies the current frame. An empty list (nothing changed)
// copies and converts the band only.
//
// Rectangles are widened outward to even coordinates (an NV12 2x2 chroma block
// depends on all four of its pixels), clipped to the surface, and merged while
// any two overlap or touch (their bounding box: copying a few more pixels is
// cheaper than another copy command). Frames WGC dropped while the loop was
// busy are covered by taking every frame still in the pool and uniting their
// rectangles (the dirty rectangles of frame k describe k against k-1).
//
// Only a WARP capture device uses this (Capture::Modern). Measured with
// MultitapHostCaptureReadbackBenchmark on this PC (build 26100, 3424x1340):
// with the WGC device on the NVIDIA GRID P40-1Q, 46 of 200 frames carried the
// PREVIOUS change's rectangles (widened by a pixel) instead of their own while
// their pixels were current; the rectangles were not a repeat of the previous
// frame's report, so no cheap rule tells them apart, and there a whole-frame
// readback costs about 1.7 ms anyway. With the WGC device on WARP (the
// compositor and the capture both on the CPU, frames handed over
// synchronously) 675 of 675 frames were exact, including 30 ticks after the
// two-buffer pool had dropped frames. A frame that reports no rectangle is
// trusted like any other (nothing changed); the sweep band bounds the damage
// should that ever be wrong.
namespace host::capture {
struct DirtyPolicy {
  size_t maxRects = 24;            // more merged rectangles than this: full copy
  int fullAreaPercent = 60;        // merged area above this share of the surface: full copy
  uint64_t fullIntervalUs = 0;     // a full copy at least this often (0: never; the sweep band refreshes instead)
  size_t maxInputRects = 512;      // more reported rectangles than this: full copy (merge cost bound)
  int sweepBands = 64;             // the surface is re-read in this many bands, one per partial tick (0: none)
};
struct DirtyPlan {
  bool full = true;
  std::vector<PixelRect> rects;  // surface coordinates; even; pairwise non-touching
  std::optional<PixelRect> sweep;  // the sweep band of this tick (partial plans only); may overlap rects
  uint64_t area() const { uint64_t sum = 0; for (const auto& r : rects) sum += uint64_t(r.width()) * uint64_t(r.height()); return sum; }
};
// Band `index` (any value; taken modulo `bands`) of an even width x height
// surface: even rows, the last band takes the remainder. nullopt: no bands or
// an empty surface.
inline std::optional<PixelRect> sweepBand(int index, int width, int height, int bands) {
  if (bands <= 0 || width <= 0 || height <= 0) return std::nullopt;
  const int rows = std::max(2, ((height + bands - 1) / bands + 1) & ~1);  // even rows per band
  const int count = (height + rows - 1) / rows;                             // bands actually needed
  const int which = ((index % count) + count) % count;
  const int top = which * rows, bottom = std::min(height, top + rows);
  return PixelRect{0, top, width, bottom};
}
// Whether two rectangles overlap or share an edge or corner.
inline bool touches(const PixelRect& a, const PixelRect& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}
inline PixelRect unite(const PixelRect& a, const PixelRect& b) {
  return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
// The rectangle widened outward to even coordinates and clipped to the even
// width x height surface; nullopt when nothing of it lies inside.
inline std::optional<PixelRect> evenClipped(const PixelRect& r, int width, int height) {
  if (r.empty() || width <= 0 || height <= 0) return std::nullopt;
  const int64_t left = std::max<int64_t>(0, int64_t(r.left) & ~int64_t(1)), top = std::max<int64_t>(0, int64_t(r.top) & ~int64_t(1));
  const int64_t right = std::min<int64_t>(width, (int64_t(r.right) + 1) & ~int64_t(1)), bottom = std::min<int64_t>(height, (int64_t(r.bottom) + 1) & ~int64_t(1));
  if (left >= right || top >= bottom) return std::nullopt;
  return PixelRect{int(left), int(top), int(right), int(bottom)};
}
// Even, clipped, merged copies of `rects` (no two touch). Rectangles outside
// the surface are dropped; an empty input gives an empty list.
inline std::vector<PixelRect> mergeDirtyRects(std::span<const PixelRect> rects, int width, int height) {
  std::vector<PixelRect> merged;
  for (const auto& r : rects) if (const auto even = evenClipped(r, width, height)) merged.push_back(*even);
  for (bool again = true; again;) {
    again = false;
    for (size_t i = 0; i < merged.size() && !again; ++i)
      for (size_t j = i + 1; j < merged.size(); ++j)
        if (touches(merged[i], merged[j])) { merged[i] = unite(merged[i], merged[j]); merged.erase(merged.begin() + ptrdiff_t(j)); again = true; break; }
  }
  return merged;
}
struct DirtyInput {
  bool stagingComplete = false;      // the staging texture holds a complete frame
  bool known = false;                // dirty rectangles known for every frame taken this tick
  std::span<const PixelRect> rects;  // their union, surface coordinates (any alignment)
  int width = 0, height = 0;         // surface size (even)
  uint64_t sinceFullUs = 0;          // time since the last full copy
  int sweepIndex = 0;                // partial ticks so far (selects the sweep band)
  DirtyPolicy policy;
};
inline DirtyPlan planDirtyCopy(const DirtyInput& in) {
  DirtyPlan plan;
  if (!in.stagingComplete || !in.known || in.width <= 0 || in.height <= 0 || in.rects.size() > in.policy.maxInputRects || (in.policy.fullIntervalUs && in.sinceFullUs >= in.policy.fullIntervalUs)) return plan;
  plan.rects = mergeDirtyRects(in.rects, in.width, in.height);
  if (plan.rects.size() > in.policy.maxRects) { plan.rects.clear(); return plan; }
  const uint64_t surface = uint64_t(in.width) * uint64_t(in.height);
  if (plan.area() * 100 > surface * uint64_t(std::clamp(in.policy.fullAreaPercent, 0, 100))) { plan.rects.clear(); return plan; }
  plan.full = false;
  plan.sweep = sweepBand(in.sweepIndex, in.width, in.height, in.policy.sweepBands);
  return plan;
}
}
