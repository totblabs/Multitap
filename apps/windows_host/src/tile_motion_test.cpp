// motion-rest-v1 (tile_motion.hpp): when lossless tiles rest for a picture that keeps moving almost everywhere,
// and when they come back. Pure: no capture, encoder or network.
#include "tile_motion.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace host;
namespace {
unsigned checks = 0;
void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
using Change = TileMotion::Change;
// `seconds` seconds of `fps` capture ticks, `changed` of each second's ticks changing over `share` of the picture.
// Each second's ticks start on the second; the next second's first tick (here an unchanged one) closes the last.
std::vector<Change> run(TileMotion& motion, uint64_t& now, int seconds, int fps, int changed, std::optional<double> share, bool moved = false) {
  std::vector<Change> seen;
  const auto note = [&](Change change) { if (change != Change::none) seen.push_back(change); };
  for (int s = 0; s < seconds; ++s) {
    for (int t = 0; t < fps; ++t) note(motion.tick(now + uint64_t(t) * 1000000 / uint64_t(fps), t < changed, t < changed ? share : std::nullopt, moved && t == 0));
    now += 1000000;
  }
  note(motion.tick(now, false));
  return seen;
}

void gameRests() {
  // A game: 120 changed pictures a second over 90 % of it. The tiles rest after two such seconds.
  TileMotion motion; uint64_t now = 1000000;
  auto seen = run(motion, now, 1, 120, 120, 0.9);
  require(seen.empty() && !motion.resting(), "One moving second rested the tiles");
  seen = run(motion, now, 2, 120, 120, 0.9);
  require(seen.size() == 1 && seen[0] == Change::began && motion.resting() && motion.rests() == 1, "Two moving seconds did not rest the tiles");
  // Resting, no share is measured: the game goes on and the tiles stay at rest.
  seen = run(motion, now, 30, 120, 120, std::nullopt);
  require(seen.empty() && motion.resting(), "A game that goes on brought the tiles back");
  // What the count alone cannot see: a part that keeps moving at 15 a second or more keeps the rest.
  seen = run(motion, now, 5, 60, 30, std::nullopt);
  require(seen.empty() && motion.resting(), "The rest ended without a still second");
  // The game stops (a still menu): one quiet second brings them back.
  seen = run(motion, now, 1, 60, 5, std::nullopt);
  require(seen.size() == 1 && seen[0] == Change::ended && !motion.resting(), "A still picture did not bring the tiles back");
}

void workKeepsTiles() {
  // A still desktop with a small moving part (a video in a window, a spinner): 60 changes a second over 6 %.
  TileMotion motion; uint64_t now = 1000000;
  require(run(motion, now, 20, 60, 60, 0.06).empty() && !motion.resting(), "A small moving part rested the tiles");
  // Typing and a cursor: few changes.
  require(run(motion, now, 20, 120, 12, 0.02).empty() && !motion.resting(), "Typing rested the tiles");
  // A scroll sent as moves keeps the tiles, however long and wide it is.
  require(run(motion, now, 10, 120, 120, 0.95, true).empty() && !motion.resting(), "A scroll sent as moves rested the tiles");
  // A video at full screen at 30 a second stays under the rate.
  require(run(motion, now, 10, 60, 30, 1.0).empty() && !motion.resting(), "A 30-a-second picture rested the tiles");
  // A 60-a-second video beside a document: half the picture keeps changing, the text beside it keeps its tiles.
  require(run(motion, now, 20, 120, 60, 0.5).empty() && !motion.resting(), "A video beside a document rested the tiles");
  // Unmeasured seconds (tiles not wanted: 게임 마우스, or no tile source yet) never rest them.
  require(run(motion, now, 10, 120, 120, std::nullopt).empty() && !motion.resting(), "Seconds without a tile measure rested the tiles");
}

void runsAndEdges() {
  // A moving second, a calm one, a moving one: no two in a row, no rest.
  TileMotion motion; uint64_t now = 1000000;
  run(motion, now, 1, 120, 120, 0.9); run(motion, now, 1, 120, 20, 0.9); run(motion, now, 1, 120, 120, 0.9);
  require(!motion.resting(), "Moving seconds that were not in a row rested the tiles");
  // Exactly at the bounds: 40 changes over 0.6 rest; 39 do not; under 15 a second brings them back, 15 does not.
  TileMotion edge; uint64_t at = 1000000;
  run(edge, at, 2, 120, 39, 0.6); require(!edge.resting(), "39 changes a second rested the tiles");
  run(edge, at, 2, 120, 40, 0.6); require(edge.resting(), "40 changes over 0.6 did not rest the tiles");
  run(edge, at, 1, 120, 15, std::nullopt); require(edge.resting(), "15 changes a second brought the tiles back");
  run(edge, at, 1, 120, 14, std::nullopt); require(!edge.resting(), "14 changes a second did not bring the tiles back");
  TileMotion narrow; uint64_t n = 1000000;
  run(narrow, n, 4, 120, 120, 0.59); require(!narrow.resting(), "A change over 59 % rested the tiles");
}

void records() {
  // The first rest is recorded with its count; a change within 10 s of the last record is not; after it, it is.
  TileMotion motion; uint64_t now = 1000000;
  auto seen = run(motion, now, 2, 120, 120, 0.9);
  require(seen.size() == 1, "No rest"); auto value = motion.record(seen[0], now);
  require(value && *value == (1u | (1u << 8)), "The first rest was not recorded with its count");
  seen = run(motion, now, 1, 60, 0, std::nullopt);
  require(seen.size() == 1 && seen[0] == Change::ended && !motion.record(seen[0], now), "An end within 10 s was recorded");
  now += 10000000;
  seen = run(motion, now, 2, 120, 120, 0.9);
  value = motion.record(seen.at(0), now);
  require(value && *value == (1u | (2u << 8)), "A rest after 10 s was not recorded with the session's count");
  require(!motion.record(Change::none, now + 20000000), "No change was recorded");
}
}

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"game-rests", gameRests}, {"work-keeps-tiles", workKeepsTiles}, {"runs-and-edges", runsAndEdges}, {"records", records}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::fprintf(stderr, "%s: %s\n", value.name, error.what()); }
  std::printf("{\"kind\":\"tile-motion\",\"passed\":%u,\"failed\":%u,\"checks\":%u}\n", passed, failed, checks);
  return failed ? 1 : 0;
}
