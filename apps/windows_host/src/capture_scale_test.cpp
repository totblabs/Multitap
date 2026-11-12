// AreaScaler (scaled capture path): SIMD output equals the scalar statement of
// the same fixed-point arithmetic, stays within a small bound of the exact
// floating-point area average, keeps flat colour exact, and is an identity at
// 1:1. Synthetic pixels only.
#include "capture_scale.hpp"
#include <cmath>
#include <iostream>
#include <random>

namespace {
uint64_t checks = 0;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); ++checks; }

std::vector<uint8_t> image(int width, int height, size_t pitch, uint32_t seed, bool text) {
  std::vector<uint8_t> pixels(pitch * height, 0xcd);
  std::mt19937 random(seed);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      auto* p = pixels.data() + size_t(y) * pitch + size_t(x) * 4;
      if (text) { const bool ink = ((x / 3 + y / 5) % 7 == 0) || (random() % 11 == 0); p[0] = ink ? 20 : 250; p[1] = ink ? 30 : 248; p[2] = ink ? 200 : 245; p[3] = 255; }
      else for (int c = 0; c < 4; ++c) p[c] = uint8_t(random());
    }
  return pixels;
}
// Exact area average in double precision.
double exact(const std::vector<uint8_t>& source, size_t pitch, int sw, int sh, int tw, int th, int x, int y, int c) {
  const double sx = double(sw) / tw, sy = double(sh) / th, x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
  double sum = 0;
  for (int j = int(std::floor(y0)); j < std::min(sh, int(std::ceil(y1))); ++j) {
    const double wy = std::min<double>(j + 1, y1) - std::max<double>(j, y0); if (wy <= 0) continue;
    for (int i = int(std::floor(x0)); i < std::min(sw, int(std::ceil(x1))); ++i) {
      const double wx = std::min<double>(i + 1, x1) - std::max<double>(i, x0); if (wx <= 0) continue;
      sum += wx * wy * source[size_t(j) * pitch + size_t(i) * 4 + c];
    }
  }
  return sum / (sx * sy);
}
void geometry(int sw, int sh, int tw, int th, uint32_t seed) {
  const size_t sourcePitch = size_t(sw) * 4 + 12, targetPitch = size_t(tw) * 4 + 20;
  host::capture::AreaScaler scaler(sw, sh, tw, th);
  for (const bool text : {false, true}) {
    const auto source = image(sw, sh, sourcePitch, seed, text);
    std::vector<uint8_t> simd(targetPitch * th, 0x5a), reference(targetPitch * th, 0x5a);
    scaler.scale(source.data(), sourcePitch, simd.data(), targetPitch);
    scaler.scaleReference(source.data(), sourcePitch, reference.data(), targetPitch);
    require(simd == reference, "SIMD scale differs from the scalar statement (or wrote the row padding)");
    double worst = 0, total = 0; size_t count = 0;
    const int stepX = std::max(1, tw / 97), stepY = std::max(1, th / 89);
    for (int y = 0; y < th; y += stepY) for (int x = 0; x < tw; x += stepX) for (int c = 0; c < 4; ++c) {
      const double error = std::abs(double(simd[size_t(y) * targetPitch + size_t(x) * 4 + c]) - exact(source, sourcePitch, sw, sh, tw, th, x, y, c));
      worst = std::max(worst, error); total += error; ++count;
    }
    require(worst <= 2.5 && total / double(count) <= 0.6, "Scaled pixels differ too much from the exact area average");
  }
  // Flat colour stays exactly that colour.
  std::vector<uint8_t> flat(sourcePitch * sh), out(targetPitch * th);
  for (int y = 0; y < sh; ++y) for (int x = 0; x < sw; ++x) { auto* p = flat.data() + size_t(y) * sourcePitch + size_t(x) * 4; p[0] = 17; p[1] = 200; p[2] = 255; p[3] = 255; }
  scaler.scale(flat.data(), sourcePitch, out.data(), targetPitch);
  for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) { const auto* p = out.data() + size_t(y) * targetPitch + size_t(x) * 4; require(p[0] == 17 && p[1] == 200 && p[2] == 255 && p[3] == 255, "Flat colour changed"); }
}
}

int main() {
  try {
    // Production shapes (client maxWidth/maxHeight below the owned output),
    // odd sizes, strong reductions, mild reductions and a 1:1 identity.
    geometry(2560, 1440, 1920, 1080, 1);
    geometry(2560, 1350, 1920, 1012, 2);
    geometry(1920, 1080, 1280, 720, 3);
    geometry(1264, 760, 1262, 758, 4);
    geometry(333, 211, 100, 64, 5);
    geometry(3840, 2160, 640, 360, 6);
    geometry(17, 9, 16, 8, 7);
    geometry(40, 30, 40, 30, 8);
    geometry(16386, 2, 8194, 1, 10);
    geometry(2, 16386, 1, 8194, 11);
    {
      host::capture::AreaScaler identity(64, 48, 64, 48);
      const auto source = image(64, 48, 64 * 4, 9, false); std::vector<uint8_t> out(64 * 48 * 4);
      identity.scale(source.data(), 64 * 4, out.data(), 64 * 4);
      require(out == source, "1:1 scaling is not an identity");
    }
    bool rejected = false; try { host::capture::AreaScaler invalid(0, 10, 5, 5); } catch (const std::exception&) { rejected = true; } require(rejected, "Invalid geometry accepted");
    std::cout << "{\"test\":\"capture-scale\",\"checks\":" << checks << ",\"result\":\"pass\"}" << std::endl;
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
