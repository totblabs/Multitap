#include "../../../native/windows_video/lossless_tiles.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

// tiles-v2 on the client side: a move and the rectangles of its exposed strip are
// applied on the decoded frame they belong to, never earlier, and everything
// that arrives while they wait stays behind them in order.
namespace {
using namespace multitap;
int checks = 0;
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); ++checks; }

constexpr int width = 320, height = 256;            // 5 x 4 tiles
constexpr int paneX = 30, paneY = 41, paneW = 263, paneH = 190;  // edges off the tile grid
constexpr uint8_t videoByte = 0x55;

uint32_t mix(uint32_t a, uint32_t b) { uint32_t h = a * 0x9e3779b1u ^ (b + 0x85ebca6bu); h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; return h; }

// BGRA desktop: static chrome, a pane showing document rows from `offset`.
std::vector<uint8_t> desktop(int offset, uint32_t stamp = 0) {
  std::vector<uint8_t> bgra(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const bool inside = x >= paneX && x < paneX + paneW && y >= paneY && y < paneY + paneH;
    uint32_t colour = inside ? mix(uint32_t(x), uint32_t(y - paneY + offset)) : mix(uint32_t(x) + 7777u, uint32_t(y));
    if (stamp && x >= 200 && x < 216 && y >= 8 && y < 24) colour = stamp;  // a change in the chrome (tile 3)
    uint8_t* p = &bgra[(size_t(y) * width + x) * 4]; p[0] = uint8_t(colour); p[1] = uint8_t(colour >> 8); p[2] = uint8_t(colour >> 16); p[3] = 255;
  }
  return bgra;
}
// What the client must publish where a tile is present: the desktop in RGBA.
std::vector<uint8_t> rgbaOf(const std::vector<uint8_t>& bgra) {
  auto rgba = bgra;
  for (size_t n = 0; n < rgba.size(); n += 4) std::swap(rgba[n], rgba[n + 2]);
  return rgba;
}

struct Client {
  lossless::Compositor compositor;
  uint32_t sequence = 0;
  Client() { require(compositor.Configure(width, height), "configure"); }
  lossless::PayloadBuilder packet(uint64_t dropPts, const std::vector<uint16_t>& drops = {}) { return lossless::PayloadBuilder(width, height, ++sequence, dropPts, drops); }
  bool accept(lossless::PayloadBuilder& builder) { const auto bytes = builder.finish(); return compositor.Accept(bytes.data(), bytes.size()) == lossless::Compositor::Outcome::accepted && !compositor.poisoned(); }
  void everyTile(const std::vector<uint8_t>& bgra) {
    auto builder = packet(lossless::noDrop);
    const uint32_t count = lossless::tileColumns(width) * lossless::tileRows(height);
    for (uint32_t i = 0; i < count; ++i) { const auto r = lossless::tileRect(i, width, height); builder.add(uint16_t(i), &bgra[(size_t(r.y) * width + r.x) * 4], size_t(width) * 4); }
    require(accept(builder), "initial tiles");
  }
  // A decoded video frame of plain `videoByte`, composed at `pts`.
  std::vector<uint8_t> frame(uint64_t pts) {
    std::vector<uint8_t> rgba(size_t(width) * height * 4, videoByte);
    compositor.ComposeDecoded(rgba.data(), size_t(width) * 4, pts);
    return rgba;
  }
};

// The host's entries for a pane that scrolled by dy: the move, then the strip in blocks.
void scrollEntries(lossless::PayloadBuilder& builder, const std::vector<uint8_t>& current, int dy) {
  builder.move(uint16_t(paneX), uint16_t(paneY), uint16_t(paneW), uint16_t(paneH), int16_t(dy));
  const int rows = std::abs(dy), top = dy > 0 ? paneY + paneH - rows : paneY;
  for (int y = top; y < top + rows; y += 64) for (int x = paneX; x < paneX + paneW; x += 64) {
    const int w = std::min(64, paneX + paneW - x), h = std::min(64, top + rows - y);
    builder.rect(uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h), &current[(size_t(y) * width + x) * 4], size_t(width) * 4);
  }
}

void scrollIsExactOnItsFrame(int dy) {
  const std::string name = "dy " + std::to_string(dy) + ": ";
  Client client; const auto before = desktop(500), after = desktop(500 + dy);
  client.everyTile(before);
  require(client.frame(10) == rgbaOf(before), name + "settled desktop is not exact");
  auto builder = client.packet(20); scrollEntries(builder, after, dy);
  require(client.accept(builder), name + "scroll packet refused");
  require(!client.compositor.republishPending(), name + "a waiting scroll asked for a republish");
  require(client.frame(19) == rgbaOf(before), name + "the scroll showed before its frame");
  require(client.frame(20) == rgbaOf(after), name + "the scrolled pane is not exact on its frame");
  require(client.frame(21) == rgbaOf(after), name + "the scrolled pane did not stay exact");
  require(client.compositor.stats().moves == 1 && client.compositor.presentTiles() == 20, name + "tiles did not stay present");
}

void laterEntriesStayBehind() {
  Client client; const auto before = desktop(500), scrolled = desktop(517), stamped = desktop(517, 0xff00ff00u);
  client.everyTile(before);
  auto scroll = client.packet(20); scrollEntries(scroll, scrolled, 17);
  require(client.accept(scroll), "scroll packet");
  // The next tick: tile 3 (chrome) changed and came back at once (drop + add in one packet).
  const auto r = lossless::tileRect(3, width, height);
  auto next = client.packet(21, {3}); next.add(3, &stamped[(size_t(r.y) * width + r.x) * 4], size_t(width) * 4);
  require(client.accept(next), "packet behind a waiting scroll");
  require(client.frame(19) == rgbaOf(before), "an add behind a waiting scroll showed early");
  require(client.frame(20) == rgbaOf(scrolled), "frame 20 is not the scrolled desktop without the later change");
  require(client.frame(21) == rgbaOf(stamped), "the later add was lost or misplaced");
  // A drop that arrives while an add of the same tile still waits must still take it.
  Client other; other.everyTile(before);
  auto scroll2 = other.packet(20); scrollEntries(scroll2, scrolled, 17); require(other.accept(scroll2), "scroll packet 2");
  auto add = other.packet(lossless::noDrop); add.add(3, &stamped[(size_t(r.y) * width + r.x) * 4], size_t(width) * 4); require(other.accept(add), "add behind the scroll");
  auto drop = other.packet(30, {3}); require(other.accept(drop), "drop behind the add");
  auto shown = other.frame(25);
  require(shown == rgbaOf(stamped), "frame 25 does not show the queued add");
  shown = other.frame(30);
  bool video = true, rest = true; const auto expected = rgbaOf(stamped);
  for (uint32_t y = 0; y < uint32_t(height); ++y) for (uint32_t x = 0; x < uint32_t(width); ++x) for (int c = 0; c < 4; ++c) {
    const size_t at = (size_t(y) * width + x) * 4 + c; const bool inTile = x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    if (inTile) video = video && shown[at] == videoByte; else rest = rest && shown[at] == expected[at];
  }
  require(video && rest, "a drop behind a waiting add left a stale tile");
}

void refusals() {
  const auto before = desktop(500), after = desktop(517);
  { Client client; client.everyTile(before);  // nothing to wait for
    auto builder = client.packet(lossless::noDrop); scrollEntries(builder, after, 17);
    require(!client.accept(builder) && client.compositor.poisoned(), "a move without a frame was accepted"); }
  { Client client; client.everyTile(before);  // offset as tall as the rectangle
    auto builder = client.packet(20); builder.move(0, 0, 64, 64, 64);
    require(!client.accept(builder) && client.compositor.poisoned(), "a move by its whole height was accepted"); }
  { Client client; client.everyTile(before);  // outside the frame
    auto builder = client.packet(20); builder.move(300, 0, 64, 64, 3);
    require(!client.accept(builder) && client.compositor.poisoned(), "a move outside the frame was accepted"); }
  { Client client;  // over tiles the client does not have: poisons when its frame comes, and shows plain video
    auto first = client.packet(lossless::noDrop); first.add(0, before.data(), size_t(width) * 4); require(client.accept(first), "one tile");
    auto builder = client.packet(20); scrollEntries(builder, after, 17); require(client.accept(builder), "queued");
    const auto shown = client.frame(20);
    require(client.compositor.poisoned() && shown == std::vector<uint8_t>(shown.size(), videoByte), "a move over absent tiles did not end in plain video"); }
  { Client client; client.everyTile(before);  // tiles-v1 packets alone never wait
    const auto r = lossless::tileRect(7, width, height);
    auto builder = client.packet(lossless::noDrop); builder.add(7, &after[(size_t(r.y) * width + r.x) * 4], size_t(width) * 4);
    require(client.accept(builder) && client.compositor.republishPending(), "a plain add was not applied at once"); }
}
}

int main() {
  try {
    for (int dy : {1, 17, 64, 100, -1, -23, -150}) scrollIsExactOnItsFrame(dy);
    laterEntriesStayBehind();
    refusals();
    std::cout << "{\"kind\":\"lossless-moves\",\"checks\":" << checks << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
