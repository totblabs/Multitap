#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

// A vertical scroll between two composed BGRA frames, found exactly: the part of
// the picture that only moved (docs/lossless-tiles-design.md, "Moves"). Pure and
// allocation-bounded; the caller owns both frames. Nothing here guesses: a move
// is returned only for rows that compare equal byte for byte at the offset.
namespace host {
// Rows [y, y + height) x columns [x, x + width) are the moved rectangle. For
// every row r of its matched part, cur(c, r) == prev(c, r + dy): dy > 0 means
// the content moved up. The other |dy| rows of the rectangle are the exposed
// strip (at the bottom when dy > 0, at the top when dy < 0), whose pixels are new.
struct ScrollMove { int x = 0, y = 0, width = 0, height = 0, dy = 0; };
struct ScrollBox { int x = 0, y = 0, width = 0, height = 0; };
struct ScrollRows { int y = 0, end = 0; };  // rows [y, end)

inline ScrollBox scrollStrip(const ScrollMove& move) {
  const int rows = std::abs(move.dy);
  return {move.x, move.dy > 0 ? move.y + move.height - rows : move.y, move.width, rows};
}

// What the client does with its lossless layer: the rows of the rectangle shift
// by dy in place. The strip keeps stale rows; the caller overwrites it.
inline void applyScroll(uint8_t* pixels, size_t stride, const ScrollMove& move) {
  const size_t bytes = size_t(move.width) * 4, left = size_t(move.x) * 4;
  const int rows = move.height - std::abs(move.dy);
  if (move.dy > 0) for (int n = 0; n < rows; ++n) { const int y = move.y + n; std::memcpy(pixels + size_t(y) * stride + left, pixels + size_t(y + move.dy) * stride + left, bytes); }
  else for (int n = rows - 1; n >= 0; --n) { const int y = move.y - move.dy + n; std::memcpy(pixels + size_t(y) * stride + left, pixels + size_t(y + move.dy) * stride + left, bytes); }
}

class ScrollDetector {
 public:
  static constexpr int band = 32;          // columns voting together
  static constexpr int maximumDy = 512;    // rows per tick; beyond it the strip is most of a screen anyway
  static constexpr int minimumRows = 48, minimumWidth = 96, minimumVotes = 24, bandVotes = 6;
  // Rows that moved together but are parted by rows where something else changed
  // (a highlight under the pointer, a caret) stay one rectangle: runs of at least
  // minimumRun rows, at most maximumGap rows apart, each with evidenceRows rows
  // that really changed (a blank area matches at any offset and proves nothing).
  static constexpr int minimumRun = 16, maximumGap = 96, evidenceRows = 8;
  // The current frame is given by the box alone: `cur` points at its pixel
  // (box.x, box.y). The box is where to look (the changed area); its right and
  // bottom are clamped to the frame. Null when no single vertical offset explains
  // a rectangle of at least the minimum size.
  std::optional<ScrollMove> detect(const uint8_t* prev, size_t prevStride, const uint8_t* cur, size_t curStride, int width, int height, ScrollBox box) {
    const auto last = last_; last_.reset(); runs_.clear();
    if (box.x < 0 || box.y < 0 || box.x >= width || box.y >= height) return std::nullopt;
    const View view{prev, prevStride, cur, curStride, box.x, box.y, std::min(box.x + box.width, width), std::min(box.y + box.height, height)};
    const int rows = view.bottom - view.top, bands = (view.right - view.left) / band;
    if (rows < minimumRows || bands * band < minimumWidth) return std::nullopt;
    // A scroll that goes on in the pane found on the last call: a few of its
    // bands and every third row give the offset (an odd step: rows doubled by a
    // scaled picture must not all fall on one of each pair). Everything after
    // the vote is the same exact work either way.
    if (last && last->x >= view.left && last->x + last->width <= view.right) {
      const int first = (last->x - view.left + band - 1) / band, end = (last->x + last->width - view.left) / band;
      if (end - first >= 3) last_ = search(view, first, end, std::max(1, (end - first) / 4), 3);
    }
    // Every third band votes (all of them in a narrow box): the offset is one
    // number for the whole rectangle, and the other bands are checked exactly.
    if (!last_) last_ = search(view, 0, bands, bands >= 12 ? 3 : 1, 1);
    if (!last_) runs_.clear();
    return last_;
  }
  // After a found move: the rows of its rectangle that only moved, in order
  // (cur(row) == prev(row + dy) across the rectangle). Every other row of the
  // rectangle is new: the exposed strip, and rows where something else changed.
  const std::vector<ScrollRows>& moved() const { return runs_; }

 private:
  struct View {
    const uint8_t* prev; size_t prevStride; const uint8_t* cur; size_t curStride; int left, top, right, bottom;
    const uint8_t* before(int x, int y) const { return prev + size_t(y) * prevStride + size_t(x) * 4; }
    const uint8_t* now(int x, int y) const { return cur + size_t(y - top) * curStride + size_t(x - left) * 4; }
  };
  // Bands first, first + step, ... < end vote with every rowStep-th current row.
  std::optional<ScrollMove> search(const View& view, int first, int end, int step, int rowStep) {
    runs_.clear();
    const int rows = view.bottom - view.top, bands = (view.right - view.left) / band, span = 2 * maximumDy + 1;
    const int voters = (end - first + step - 1) / step;
    votes_.assign(size_t(voters) * span, 0); total_.assign(span, 0);
    previous_.resize(size_t(rows)); current_.resize(size_t(rows)); sorted_.resize(size_t(rows));
    const auto sign = [&](int b, int every) {
      const int x = view.left + b * band;
      for (int r = 0; r < rows; ++r) previous_[size_t(r)] = signature(view.before(x, view.top + r));
      for (int r = 0; r < rows; r += every) current_[size_t(r)] = signature(view.now(x, view.top + r));
    };
    for (int n = 0; n < voters; ++n) {
      sign(first + n * step, rowStep);
      for (int r = 0; r < rows; ++r) sorted_[size_t(r)] = {previous_[size_t(r)], r};
      std::sort(sorted_.begin(), sorted_.end());
      for (int r = 0; r < rows; r += rowStep) {
        // A row that did not change says nothing; neither does one that is
        // everywhere (blank lines, a uniform background).
        const uint64_t value = current_[size_t(r)];
        if (value == previous_[size_t(r)]) continue;
        auto at = std::lower_bound(sorted_.begin(), sorted_.end(), std::pair<uint64_t, int>{value, 0});
        int same = 0;
        for (auto probe = at; probe != sorted_.end() && probe->first == value && same <= 4; ++probe) ++same;
        if (!same || same > 4) continue;
        for (; same; --same, ++at) {
          const int dy = at->second - r;
          if (dy && std::abs(dy) <= maximumDy) { ++votes_[size_t(n) * span + size_t(dy + maximumDy)]; ++total_[size_t(dy + maximumDy)]; }
        }
      }
    }
    const int best = int(std::max_element(total_.begin(), total_.end()) - total_.begin());
    if (total_[size_t(best)] < minimumVotes) return std::nullopt;
    const int dy = best - maximumDy;
    // The band that saw this offset best: its rows that changed and moved by it
    // are the evidence, and up to 16 of them decide which bands belong.
    int witness = 0;
    for (int n = 1; n < voters; ++n) if (votes_[size_t(n) * span + size_t(best)] > votes_[size_t(witness) * span + size_t(best)]) witness = n;
    sign(first + witness * step, 1);
    samples_.clear(); proof_.assign(size_t(rows) + 1, 0);
    for (int r = 0; r < rows; ++r) {
      const bool moved = r + dy >= 0 && r + dy < rows && current_[size_t(r)] != previous_[size_t(r)] && current_[size_t(r)] == previous_[size_t(r + dy)];
      if (moved) samples_.push_back(view.top + r);
      proof_[size_t(r) + 1] = proof_[size_t(r)] + (moved ? 1 : 0);
    }
    if (samples_.size() < size_t(bandVotes)) return std::nullopt;
    if (samples_.size() > 16) { for (size_t n = 0; n < 16; ++n) samples_[n] = samples_[n * samples_.size() / 16]; samples_.resize(16); }
    // The longest run of neighbouring bands that agree on this offset.
    int runStart = -1, runLength = 0;
    for (int b = 0, start = -1; b <= bands; ++b) {
      bool agrees = b < bands;
      for (size_t n = 0; agrees && n < samples_.size(); ++n)
        agrees = !std::memcmp(view.now(view.left + b * band, samples_[n]), view.before(view.left + b * band, samples_[n] + dy), size_t(band) * 4);
      if (agrees && start < 0) start = b;
      if (!agrees && start >= 0) { if (b - start > runLength) { runStart = start; runLength = b - start; } start = -1; }
    }
    if (runLength * band < minimumWidth) return std::nullopt;
    int x0 = view.left + runStart * band, x1 = x0 + runLength * band;
    // Exact rows: both the row and its source lie inside the box.
    const int firstRow = std::max(view.top, view.top - dy), lastRow = std::min(view.bottom, view.bottom - dy);
    all_.clear();
    for (int y = firstRow, start = -1; y <= lastRow; ++y) {
      const bool equal = y < lastRow && !std::memcmp(view.now(x0, y), view.before(x0, y + dy), size_t(x1 - x0) * 4);
      if (equal && start < 0) start = y;
      if (!equal && start >= 0) { if (y - start >= minimumRun) all_.push_back({start, y}); start = -1; }
    }
    if (all_.empty()) return std::nullopt;
    const auto evidence = [&](const ScrollRows& run) { return proof_[size_t(run.end - view.top)] - proof_[size_t(run.y - view.top)]; };
    // The run with the most evidence, then its neighbours across short gaps.
    size_t low = 0;
    for (size_t n = 1; n < all_.size(); ++n) {
      const int a = evidence(all_[n]), b = evidence(all_[low]);
      if (a > b || (a == b && all_[n].end - all_[n].y > all_[low].end - all_[low].y)) low = n;
    }
    if (!evidence(all_[low])) return std::nullopt;
    // A run without enough evidence between two that have it (the few rows of a
    // highlight that happen to match) does not end the search; it is exact too.
    size_t high = low;
    for (size_t n = low; n-- > 0 && all_[low].y - all_[n].end <= maximumGap;) if (evidence(all_[n]) >= evidenceRows) low = n;
    for (size_t n = high + 1; n < all_.size() && all_[n].y - all_[high].end <= maximumGap; ++n) if (evidence(all_[n]) >= evidenceRows) high = n;
    runs_.assign(all_.begin() + std::ptrdiff_t(low), all_.begin() + std::ptrdiff_t(high) + 1);
    const int y0 = runs_.front().y, y1 = runs_.back().end;
    if (y1 - y0 < minimumRows) return std::nullopt;
    // The bands are a 32-column grid: widen to the real edges, column by column
    // (the next band did not agree, so the edge lies inside it).
    const auto column = [&](int x) {
      for (const auto& run : runs_) for (int y = run.y; y < run.end; ++y) if (std::memcmp(view.now(x, y), view.before(x, y + dy), 4)) return false;
      return true;
    };
    for (int n = 1; n < band && x0 > view.left && column(x0 - 1); ++n) --x0;
    for (int n = 1; n < band && x1 < view.right && column(x1); ++n) ++x1;
    return ScrollMove{x0, dy > 0 ? y0 : y0 + dy, x1 - x0, y1 - y0 + std::abs(dy), dy};
  }
  static uint64_t signature(const uint8_t* row) {
    uint64_t hash = 0x9e3779b97f4a7c15ull;
    for (int n = 0; n < band * 4; n += 8) { uint64_t value; std::memcpy(&value, row + n, 8); hash = (hash ^ value) * 0xff51afd7ed558ccdull; hash ^= hash >> 32; }
    return hash;
  }
  std::vector<int> votes_, total_, samples_, proof_;
  std::vector<uint64_t> previous_, current_;
  std::vector<std::pair<uint64_t, int>> sorted_;
  std::vector<ScrollRows> all_, runs_;
  std::optional<ScrollMove> last_;
};
}
