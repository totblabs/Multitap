// Reference vectors for the Android port of tiles-v1 (LosslessTiles.kt): the
// Host's PayloadBuilder encodes, the Windows client's Compositor composes, and
// the JVM test (LosslessTilesTest) must reach the same present tiles and the
// same composed pixels. Regenerate after any change to lossless_tiles.hpp:
//   zig c++ -std=c++17 -O1 -I native/windows_video apps/windows_client/test/native/lossless_tiles_vectors.cpp -o vectors.exe
//   vectors.exe > apps/windows_client/android/app/src/test/resources/lossless_tiles_vectors.txt
#include <lossless_tiles.hpp>
#include <cstdio>
#include <vector>

using namespace multitap;

namespace {
constexpr uint32_t W = 144, H = 80;  // 3 x 2 tiles, partial right column and bottom row

// Screen-like BGRA: flat areas, text-like strokes, a gradient and one noisy block.
std::vector<uint8_t> picture(uint32_t seed) {
  std::vector<uint8_t> bgra(size_t(W) * H * 4);
  uint32_t random = 0x9e3779b9u ^ seed;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      uint8_t* p = bgra.data() + (size_t(y) * W + x) * 4;
      uint8_t r = 240, g = 240, b = 236;
      if ((y % 9 == 3 || x % 7 == (seed % 7)) && x > 4 && y > 4) { r = 20; g = 24; b = uint8_t(40 + seed * 30); }
      if (x >= 64 && x < 128 && y < 64) { r = uint8_t(x * 2 + seed); g = uint8_t(y * 3); b = uint8_t((x + y) & 255); }
      if (x >= 128 && y >= 64) { random = random * 1664525u + 1013904223u; r = uint8_t(random >> 24); g = uint8_t(random >> 16); b = uint8_t(random >> 8); }
      p[0] = b; p[1] = g; p[2] = r; p[3] = 0x7f;  // alpha is not coded
    }
  return bgra;
}

std::vector<uint8_t> packet(uint32_t sequence, uint64_t dropPts, const std::vector<uint16_t>& drops, const std::vector<uint16_t>& adds, const std::vector<uint8_t>& bgra) {
  lossless::PayloadBuilder builder(W, H, sequence, dropPts, drops);
  for (const auto index : adds) {
    const auto rect = lossless::tileRect(index, W, H);
    builder.add(index, bgra.data() + (size_t(rect.y) * W + rect.x) * 4, size_t(W) * 4);
  }
  return builder.finish();
}

uint64_t fnv(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (const auto byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
  return hash;
}

void emitPacket(const std::vector<uint8_t>& bytes, lossless::Compositor& compositor) {
  std::printf("packet ");
  for (const auto byte : bytes) std::printf("%02x", byte);
  std::printf(" %d\n", int(compositor.Accept(bytes.data(), bytes.size())));
}

// The video under the tiles: a flat RGBA picture of the video size, composed
// at `pts` as the Windows engine publishes it: a scaled source gets a
// source-size frame (ComposeDecodedScaled: the flat video upscales to itself),
// otherwise the video-size frame. "-" when no tile size is known yet.
void emitState(lossless::Compositor& compositor, uint64_t pts) {
  const uint32_t vw = compositor.videoWidth(), vh = compositor.videoHeight();
  std::vector<uint8_t> frame(size_t(vw) * vh * 4);
  for (size_t i = 0; i < frame.size(); i += 4) { frame[i] = 0x10; frame[i + 1] = 0x20; frame[i + 2] = 0x30; frame[i + 3] = 0xff; }
  if (compositor.scaled()) {
    std::vector<uint8_t> source(size_t(compositor.width()) * compositor.height() * 4);
    compositor.ComposeDecodedScaled(frame.data(), size_t(vw) * 4, source.data(), size_t(compositor.width()) * 4, pts);
    frame = std::move(source);
  } else compositor.ComposeDecoded(frame.data(), size_t(vw) * 4, pts);
  std::printf("state %llu ", static_cast<unsigned long long>(pts));
  const uint32_t tiles = compositor.width() ? lossless::tileColumns(compositor.width()) * lossless::tileRows(compositor.height()) : 0;
  if (!tiles) std::printf("-");
  for (uint32_t i = 0; i < tiles; ++i) std::printf("%d", compositor.present(i) ? 1 : 0);
  std::printf(" %016llx\n", static_cast<unsigned long long>(fnv(frame)));
}
}  // namespace

int main() {
  const auto a = picture(1), b = picture(2);
  lossless::Compositor compositor;
  if (!compositor.Configure(W, H, false)) return 1;
  std::printf("# lossless_tiles_vectors.cpp from native/windows_video/lossless_tiles.hpp\n");
  std::printf("frame %u %u\n", W, H);
  emitPacket(packet(1, 1000, {}, {0, 1, 2, 3, 4, 5}, a), compositor);
  emitState(compositor, 999);
  emitPacket(packet(2, 2000, {1, 4}, {2}, b), compositor);
  emitState(compositor, 1999);
  emitState(compositor, 2000);
  // A drop and an add of the same tile: the newer content replaces the drop;
  // a drop of a tile that is not present changes nothing.
  emitPacket(packet(3, 3000, {0, 1}, {0, 5}, b), compositor);
  emitState(compositor, 3000);
  emitState(compositor, 5000);
  // A sequence gap poisons: nothing is composited again.
  emitPacket(packet(5, 6000, {}, {1}, a), compositor);
  emitState(compositor, 6000);
  // Scaled-capable compositors (Configure(..., true), the legacy engine's):
  // "scaled <video width> <video height>" starts one; the same 144 x 80 tiles
  // then describe the source of a downscaled video (128 x 72), of a video at
  // the source size (unscaled), or of a larger video (refused: poisoned).
  for (const auto& video : {std::pair<uint32_t, uint32_t>{128, 72}, {144, 80}, {160, 96}}) {
    lossless::Compositor scaled;
    if (!scaled.Configure(video.first, video.second, true)) return 1;
    std::printf("scaled %u %u\n", video.first, video.second);
    emitState(scaled, 500);  // no tile packet yet: the video as it is
    emitPacket(packet(1, 1000, {}, {0, 1, 2, 3, 4, 5}, a), scaled);
    emitState(scaled, 999);
    emitPacket(packet(2, 2000, {1, 4}, {2}, b), scaled);
    emitState(scaled, 1999);
    emitState(scaled, 2000);
    emitPacket(packet(3, 3000, {0, 1}, {0, 5}, b), scaled);
    emitState(scaled, 3000);
    emitPacket(packet(5, 6000, {}, {1}, a), scaled);
    emitState(scaled, 6000);
  }
  return 0;
}
