#include "media.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <cstring>

namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("Capture cache regression"); }
class Dib {
 public:
  Dib(int width, int height) {
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(dc && bitmap && pixels); previous = SelectObject(dc, bitmap);
  }
  ~Dib() { if (previous) SelectObject(dc, previous); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
  HDC dc = nullptr; uint8_t* pixels = nullptr;
 private: HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr;
};
std::vector<uint8_t> fixture(int width, int height) {
  std::vector<uint8_t> frame(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const auto at = (size_t(y) * width + x) * 4;
    frame[at] = uint8_t(x % 251); frame[at + 1] = uint8_t(y % 241);
    frame[at + 2] = uint8_t((x + y) % 239); frame[at + 3] = 255;
  }
  return frame;
}
double cpuMs() {
  FILETIME c{}, e{}, k{}, u{}; require(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) != FALSE);
  ULARGE_INTEGER kernel{}, user{}; kernel.LowPart = k.dwLowDateTime; kernel.HighPart = k.dwHighDateTime;
  user.LowPart = u.dwLowDateTime; user.HighPart = u.dwHighDateTime;
  return double(kernel.QuadPart + user.QuadPart) / 10000;
}
struct Measurement { double wall = 0, cpu = 0, p95 = 0; int hits = 0; };
Measurement measure(const std::vector<uint8_t>& frame, int width, int height, bool cached) {
  host::PixelConversionCache cache; std::vector<uint8_t> output(size_t(width) * height * 3 / 2);
  host::bgraToNv12(frame.data(), output.data(), width, height);
  cache.reuse(frame.data(), frame.size(), true);
  std::vector<double> timings; Measurement result; const auto cpu = cpuMs(); const auto start = host::Clock::now();
  for (int i = 0; i < 180; ++i) {
    const auto began = host::Clock::now();
    if (cached && cache.reuse(frame.data(), frame.size(), true)) ++result.hits;
    else host::bgraToNv12(frame.data(), output.data(), width, height);
    timings.push_back(std::chrono::duration<double,std::milli>(host::Clock::now() - began).count());
  }
  result.wall = std::chrono::duration<double,std::milli>(host::Clock::now() - start).count();
  result.cpu = cpuMs() - cpu; std::sort(timings.begin(), timings.end()); result.p95 = timings[170];
  return result;
}
void print(const Measurement& value) {
  std::cout << "{\"wallMs\":" << value.wall << ",\"cpuMs\":" << value.cpu << ",\"p95Ms\":" << value.p95 << ",\"hits\":" << value.hits << '}';
}
}
int main() {
 try {
  constexpr int width = 640, height = 360;
  auto base = fixture(width, height); Dib dib(width, height); host::PixelConversionCache cache;
  std::vector<uint8_t> expected(size_t(width)*height*3/2), actual(expected.size()); int cases = 0;
  const auto check = [&](bool eligible) {
    host::bgraToNv12(dib.pixels, expected.data(), width, height);
    if (!cache.reuse(dib.pixels, base.size(), eligible)) host::bgraToNv12(dib.pixels, actual.data(), width, height);
    require(actual == expected); ++cases;
  };
  std::memcpy(dib.pixels, base.data(), base.size()); check(false); check(true); check(true);
  // Same backing DIB, actual Windows cursor drawing: motion, shape, hide, clipping.
  for (const auto point : {POINT{40,50}, POINT{160,100}, POINT{-12,25}, POINT{630,350}}) {
    std::memcpy(dib.pixels, base.data(), base.size());
    require(DrawIconEx(dib.dc, point.x, point.y, LoadCursorW(nullptr, IDC_ARROW),32,32,0,nullptr,DI_NORMAL) != FALSE); GdiFlush(); check(false); check(true); check(true);
  }
  for (const auto icon : {IDC_WAIT, IDC_IBEAM}) {
    std::memcpy(dib.pixels, base.data(), base.size());
    require(DrawIconEx(dib.dc,60,60,LoadCursorW(nullptr,icon),32,32,0,nullptr,DI_NORMAL) != FALSE); GdiFlush(); check(true); check(true);
  }
  std::memcpy(dib.pixels, base.data(), base.size()); check(true); check(true);
  // Conservative misses for alpha-only mutation, dirty WGC, and unknown cursor.
  dib.pixels[3] ^= 1; check(true); check(false); check(true); check(true);
  for (size_t at : {size_t(0), base.size()/2, base.size()-1}) { dib.pixels[at] ^= 3; check(true); check(true); }
  auto resized = fixture(320,180); require(!cache.reuse(resized.data(),resized.size(),true)); ++cases;
  int rejected = 0; for (const auto bytes : {size_t(0), size_t(3), size_t(3840ull*2160*4+4)}) {
    try { cache.reuse(base.data(),bytes,true); } catch (const std::invalid_argument&) { ++rejected; }
  }
  require(rejected == 3);
  std::cout << "{\"scope\":\"Final BGRA conversion step only; synthetic frames and owned memory DIB/cursor; no actual display capture\",\"byteEquivalentCases\":" << cases << ",\"invalidBoundsRejected\":" << rejected << ",\"samples\":180,\"measurements\":[";
  bool first = true; for (const auto size : {POINT{1920,1080},POINT{2560,1440},POINT{3840,1080}}) {
    const auto frame = fixture(size.x,size.y); const auto normal = measure(frame,size.x,size.y,false), reused = measure(frame,size.x,size.y,true);
    if (!first) std::cout << ','; first = false;
    std::cout << "{\"width\":" << size.x << ",\"height\":" << size.y << ",\"alwaysConvert\":"; print(normal); std::cout << ",\"reuseUnchanged\":"; print(reused); std::cout << '}';
  }
  std::cout << "],\"serviceChanges\":0,\"driverChanges\":0,\"vmAccessed\":false}\n";
 } catch (...) { std::cerr << "Capture cache QA failed\n"; return 1; }
}
