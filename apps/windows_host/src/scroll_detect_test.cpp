#include "scroll_detect.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// Synthetic desktop: static chrome around a pane of text lines that scrolls.
// Every expectation is exact: a detected move, applied to the previous frame
// with the rows it does not explain copied from the current one, must reproduce
// the current frame.
namespace {
using host::ScrollBox;
using host::ScrollDetector;
using host::ScrollMove;
using host::ScrollRows;

int cases = 0;
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); }

uint32_t mix(uint32_t a, uint32_t b) { uint32_t h = a * 0x9e3779b1u ^ (b + 0x85ebca6bu); h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; return h; }

struct Scene {
  int width = 0, height = 0;
  ScrollBox pane;
  bool uniformChrome = false;
  // The pane shows document rows [offset, offset + pane.height). `highlight`:
  // 24 rows at that distance from the pane's top are tinted, wherever the text is
  // (the line under the pointer).
  std::vector<uint8_t> frame(int offset, int cursorX = -1, int highlight = -1) const {
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      uint32_t colour;
      const bool inside = x >= pane.x && x < pane.x + pane.width && y >= pane.y && y < pane.y + pane.height;
      if (inside) {
        const int row = y - pane.y + offset;
        // 20-row lines: 12 rows of glyph-like ink, 8 blank rows.
        const bool ink = row % 20 < 12 && mix(uint32_t(x / 2), uint32_t(row / 2)) % 4 == 0;
        const bool tinted = highlight >= 0 && y - pane.y >= highlight && y - pane.y < highlight + 24;
        colour = ink ? 0xff202020u + (mix(uint32_t(row / 20), 7) % 3) * 0x00300000u : tinted ? 0xffcce8ffu : 0xffffffffu;
      } else colour = uniformChrome ? 0xfff0f0f0u : 0xff000000u | (mix(uint32_t(x), uint32_t(y) + 99991u) & 0x00ffffffu);
      if (cursorX >= 0 && x >= cursorX && x < cursorX + 12 && y >= pane.y + 30 && y < pane.y + 49) colour = 0xff0000ffu;
      std::memcpy(&pixels[(size_t(y) * width + x) * 4], &colour, 4);
    }
    return pixels;
  }
  ScrollBox whole() const { return {0, 0, width, height}; }
  std::optional<ScrollMove> detect(ScrollDetector& detector, const std::vector<uint8_t>& previous, const std::vector<uint8_t>& current, ScrollBox box) const {
    const size_t stride = size_t(width) * 4;
    if (box.x < 0 || box.y < 0 || box.x >= width || box.y >= height) return detector.detect(previous.data(), stride, current.data(), stride, width, height, box);
    return detector.detect(previous.data(), stride, current.data() + size_t(box.y) * stride + size_t(box.x) * 4, stride, width, height, box);
  }
  std::optional<ScrollMove> detect(ScrollDetector& detector, const std::vector<uint8_t>& previous, const std::vector<uint8_t>& current) const { return detect(detector, previous, current, whole()); }
};

// The client's result: the previous frame, the move, then every row of the
// rectangle that did not only move, from the current frame.
std::vector<uint8_t> replay(const std::vector<uint8_t>& previous, const std::vector<uint8_t>& current, int width, const ScrollMove& move, const std::vector<ScrollRows>& moved) {
  auto layer = previous; const size_t stride = size_t(width) * 4;
  host::applyScroll(layer.data(), stride, move);
  for (int y = move.y; y < move.y + move.height; ++y) {
    bool explained = false;
    for (const auto& run : moved) explained = explained || (y >= run.y && y < run.end);
    if (!explained) std::memcpy(&layer[size_t(y) * stride + size_t(move.x) * 4], &current[size_t(y) * stride + size_t(move.x) * 4], size_t(move.width) * 4);
  }
  return layer;
}
std::string text(const ScrollMove& move) { return std::to_string(move.x) + "," + std::to_string(move.y) + " " + std::to_string(move.width) + "x" + std::to_string(move.height) + " by " + std::to_string(move.dy); }
bool isPane(const Scene& scene, const ScrollMove& move) { return move.x == scene.pane.x && move.width == scene.pane.width && move.y == scene.pane.y && move.height == scene.pane.height; }

void exact(const Scene& scene, int from, int to, const std::string& name, bool exactPane = true) {
  const auto previous = scene.frame(from), current = scene.frame(to);
  ScrollDetector detector;
  const auto move = scene.detect(detector, previous, current);
  require(move.has_value(), name + ": no move found");
  require(move->dy == to - from, name + ": offset " + std::to_string(move->dy) + " instead of " + std::to_string(to - from));
  require(replay(previous, current, scene.width, *move, detector.moved()) == current, name + ": the replayed move differs from the current frame");
  if (exactPane) require(isPane(scene, *move) && detector.moved().size() == 1, name + ": rectangle " + text(*move) + " is not the pane");
  ++cases;
}
}

int main() {
  try {
    Scene scene; scene.width = 640; scene.height = 480; scene.pane = {41, 61, 557, 379};  // edges off every grid
    exact(scene, 100, 117, "up by 17");
    exact(scene, 117, 100, "down by 17");
    exact(scene, 100, 101, "up by 1");
    exact(scene, 100, 164, "up by a tile");
    exact(scene, 400, 100, "down by 300 (a fling)");
    // Uniform chrome matches at any offset: the rectangle may grow into it, the replay stays exact.
    Scene plain = scene; plain.uniformChrome = true;
    exact(plain, 100, 137, "uniform chrome", false);
    {
      ScrollDetector detector; const auto a = scene.frame(100), same = scene.frame(100);
      require(!scene.detect(detector, a, same), "an unchanged frame moved"); ++cases;
      // A new page: nothing of the old one is anywhere.
      const auto other = scene.frame(100000);
      require(!scene.detect(detector, a, other), "a page change read as a move"); ++cases;
      // More than the detector looks for.
      const auto far = scene.frame(100 + ScrollDetector::maximumDy + 40);
      const auto move = scene.detect(detector, a, far);
      require(!move || replay(a, far, scene.width, *move, detector.moved()) == far, "a far jump produced a wrong move"); ++cases;
      // Something else changes inside the pane while it scrolls (a caret): whatever is returned stays exact.
      const auto before = scene.frame(100, 200), after = scene.frame(123, 260);
      const auto partial = scene.detect(detector, before, after);
      if (partial) {
        const auto layer = replay(before, after, scene.width, *partial, detector.moved());
        const size_t stride = size_t(scene.width) * 4;
        for (int y = partial->y; y < partial->y + partial->height; ++y)
          require(!std::memcmp(&layer[size_t(y) * stride + size_t(partial->x) * 4], &after[size_t(y) * stride + size_t(partial->x) * 4], size_t(partial->width) * 4), "a move next to another change is not exact inside its rectangle");
      }
      ++cases;
      // A box whose far edges leave the frame is clamped; one too small or outside finds nothing.
      require(!scene.detect(detector, a, same, {600, 400, 4000, 4000}), "a clamped box moved");
      require(!scene.detect(detector, a, scene.frame(117), {0, 0, 64, 32}), "a tiny box moved");
      require(!scene.detect(detector, a, scene.frame(117), {-4, 0, 640, 480}) && !scene.detect(detector, a, scene.frame(117), {0, 480, 640, 480}), "a box outside the frame moved");
      ++cases;
    }
    {
      // The line under the pointer is tinted and stays where the pointer is while the
      // text moves under it: still one rectangle, the pane, with the rows around the
      // tint left to be sent as they are.
      for (int dy : {17, -9, 1, -2, 40}) {
        ScrollDetector detector; const auto before = scene.frame(200, -1, 150), after = scene.frame(200 + dy, -1, 150);
        const auto move = scene.detect(detector, before, after);
        require(move && move->dy == dy && isPane(scene, *move), "a highlighted line split the pane (by " + std::to_string(dy) + "): " + (move ? text(*move) : std::string("no move")));
        require(detector.moved().size() >= 2, "a highlighted line did not part the moved rows");
        require(replay(before, after, scene.width, *move, detector.moved()) == after, "the replay around a highlighted line is not exact");
      }
      ++cases;
    }
    {
      // A scroll that goes on: the same detector, offsets that change every frame
      // and a reversal, must find what a fresh detector finds.
      ScrollDetector running; int offset = 300;
      for (int dy : {3, 11, 26, 40, 7, 1, -5, -33, 64, 2}) {
        const auto before = scene.frame(offset), after = scene.frame(offset + dy);
        ScrollDetector fresh;
        const auto expected = scene.detect(fresh, before, after), move = scene.detect(running, before, after);
        require(expected && move && text(*expected) == text(*move), "a running scroll by " + std::to_string(dy) + " differs from a fresh look");
        require(replay(before, after, scene.width, *move, running.moved()) == after, "a running scroll is not exact");
        offset += dy;
      }
      // Then the page is replaced: nothing, and the next scroll is found again.
      require(!scene.detect(running, scene.frame(offset), scene.frame(900000)), "a page change after a scroll read as a move");
      const auto again = scene.detect(running, scene.frame(500), scene.frame(512));
      require(again && again->dy == 12 && isPane(scene, *again), "a scroll after a page change was not found");
      ++cases;
    }
    {
      // The current frame as the box alone, in a buffer with its own row length.
      const ScrollBox box{32, 40, 576, 420}; const size_t stride = size_t(scene.width) * 4, pitch = size_t(box.width) * 4 + 52;
      const auto before = scene.frame(100), after = scene.frame(131);
      std::vector<uint8_t> piece(pitch * size_t(box.height), 0xa5);
      for (int y = 0; y < box.height; ++y) std::memcpy(&piece[size_t(y) * pitch], &after[size_t(box.y + y) * stride + size_t(box.x) * 4], size_t(box.width) * 4);
      ScrollDetector detector;
      const auto move = detector.detect(before.data(), stride, piece.data(), pitch, scene.width, scene.height, box);
      require(move && move->dy == 31 && isPane(scene, *move), "a box-only current frame: " + (move ? text(*move) : std::string("no move")));
      ++cases;
    }
    // 1080p cost, the size the Host compares every tick: the first look, and a scroll that goes on.
    Scene large; large.width = 1920; large.height = 1080; large.pane = {0, 120, 1920, 780};
    const auto a = large.frame(500), b = large.frame(523);
    const auto time = [&](ScrollDetector* running) {
      std::optional<ScrollMove> move; constexpr int runs = 20;
      const auto started = std::chrono::steady_clock::now();
      for (int n = 0; n < runs; ++n) { ScrollDetector fresh; auto& detector = running ? *running : fresh; move = large.detect(detector, a, b); }
      return std::pair<double, std::optional<ScrollMove>>{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / runs, move};
    };
    (void)time(nullptr);
    ScrollDetector running; require(large.detect(running, a, b).has_value(), "1080p move");
    const auto first = time(nullptr), going = time(&running);
    require(first.second && going.second && first.second->dy == 23 && text(*first.second) == text(*going.second), "1080p move");
    { ScrollDetector detector; const auto move = large.detect(detector, a, b); require(move && replay(a, b, large.width, *move, detector.moved()) == b, "1080p move is not exact"); }
    std::cout << "{\"kind\":\"scroll-detect\",\"cases\":" << cases << ",\"first1080pMs\":" << first.first << ",\"going1080pMs\":" << going.first
              << ",\"rect\":[" << going.second->x << "," << going.second->y << "," << going.second->width << "," << going.second->height << "]}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
