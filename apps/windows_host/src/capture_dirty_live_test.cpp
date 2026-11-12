// capture-dirty-v1 (capture_dirty.hpp) on the real Capture (media.cpp): the
// primary monitor captured several times at once. Two captures on WARP devices
// (the kept device of a Capture; the path a GPU-less VM takes) read back only
// the reported dirty rectangles; a third WARP capture reads whole frames (the
// released path, the reference); two more capture as the Host would on this
// PC. Whenever every pool is drained and no capture reports a new frame, the
// dirty pictures must equal the whole-frame WARP picture byte for byte: the
// persistent staging and the NV12 of the dirty path are a whole-frame
// readback. The cursor is left out of all (cursor-v1), so a moving pointer
// cannot differ by timing. The desktop is read into memory only; nothing is
// saved or sent. Prints the per-tick capture and readback times.
#include "media.hpp"
#include <d3d11.h>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <thread>

namespace {
using host::Clock;
int checks = 0; std::string failures;
void require(bool ok, const std::string& what) { ++checks; if (!ok) { failures += what + "\n"; std::cerr << "FAIL: " << what << "\n"; } }
struct Series {
  std::vector<double> values;
  void add(double v) { values.push_back(v); }
  double mean() const { return values.empty() ? 0 : std::accumulate(values.begin(), values.end(), 0.0) / double(values.size()); }
  double quantile(double q) const { if (values.empty()) return 0; auto s = values; std::sort(s.begin(), s.end()); return s[std::min(s.size() - 1, size_t(q * double(s.size() - 1) + 0.5))]; }
  void json(std::ostream& out) const { out << "{\"n\":" << values.size() << ",\"mean\":" << mean() << ",\"p50\":" << quantile(.5) << ",\"p95\":" << quantile(.95) << ",\"max\":" << quantile(1) << '}'; }
};
}

int main() try {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  host::Runtime runtime;
  const int w = GetSystemMetrics(SM_CXSCREEN) & ~1, h = GetSystemMetrics(SM_CYSCREEN) & ~1;
  const RECT primary{0, 0, w, h};
  if (w < 64 || h < 64) { std::cout << "{\"kind\":\"capture-dirty-live\",\"skipped\":\"no primary monitor\"}\n"; return 0; }
  host::Com<ID3D11Device> warpDevice; host::Com<ID3D11DeviceContext> warpContext;
  host::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &warpDevice, nullptr, &warpContext), "WARP device");
  ID3D11Device* raw = warpDevice.Get();
  std::shared_ptr<ID3D11Device> kept(raw, [keep = warpDevice](ID3D11Device*) mutable { keep.Reset(); });
  host::Capture dirty(w, h, primary, kept), whole(w, h, primary);
  // Second captures of each kind: whether two captures of one kind agree with
  // each other at all (a reference for the cross-device comparison).
  host::Com<ID3D11Device> warpDevice2; host::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &warpDevice2, nullptr, nullptr), "WARP device 2");
  ID3D11Device* raw2 = warpDevice2.Get(); std::shared_ptr<ID3D11Device> kept2(raw2, [keep = warpDevice2](ID3D11Device*) mutable { keep.Reset(); });
  host::Capture dirty2(w, h, primary, kept2), whole2(w, h, primary);
  dirty2.composeCursor(false); whole2.composeCursor(false);
  // A WARP capture reading whole frames (the released path on a GPU-less VM):
  // whether a difference comes from the dirty readback or from what WGC
  // delivers to a WARP device at all.
  host::Com<ID3D11Device> warpDevice3; host::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &warpDevice3, nullptr, nullptr), "WARP device 3");
  ID3D11Device* raw3 = warpDevice3.Get(); std::shared_ptr<ID3D11Device> kept3(raw3, [keep = warpDevice3](ID3D11Device*) mutable { keep.Reset(); });
  host::Capture::dirtyReadbackEnabled = false;
  host::Capture warpWhole(w, h, primary, kept3);
  host::Capture::dirtyReadbackEnabled = true;
  warpWhole.composeCursor(false);
  require(!warpWhole.dirtyReadback() && std::string(warpWhole.backend()) == "wgc-warp", "the reference WARP capture reads whole frames");
  int warpPairMismatches = 0, gpuPairMismatches = 0, pairComparisons = 0, warpWholeVsGpu = 0, warpWholeVsDirty = 0;
  dirty.composeCursor(false); whole.composeCursor(false); dirty.observeReadback(true); whole.observeReadback(true);
  const std::string dirtyBackend = dirty.backend(), wholeBackend = whole.backend();
  require(dirtyBackend == "wgc-warp", "a kept WARP device is recognised as WARP (" + dirtyBackend + ")");
  const bool reporting = dirty.dirtyReadback();
  if (!reporting) std::cout << "{\"note\":\"this Windows does not report dirty regions; the WARP capture reads whole frames\"}\n";
  require(!whole.dirtyReadback() || wholeBackend == "wgc-warp", "a GPU device never uses dirty readback");
  Series dirtyCapture, dirtyReadback, wholeCapture, wholeReadback;
  int ticks = 0, idleComparisons = 0, idleMismatches = 0, dirtyUpdates = 0, wholeUpdates = 0;
  auto next = Clock::now();
  for (int tick = 0; tick < 400; ++tick) {
    next += std::chrono::microseconds(16667); std::this_thread::sleep_until(next);
    const auto& a = dirty.frame(); const auto& b = whole.frame(); ++ticks;
    const auto& a2 = dirty2.frame(); const auto& b2 = whole2.frame(); const auto& c = warpWhole.frame();
    if (tick >= 10 && !dirty.frameUpdated() && !whole.frameUpdated() && !dirty2.frameUpdated() && !whole2.frameUpdated() && !warpWhole.frameUpdated()) {
      ++pairComparisons; if (a != a2) ++warpPairMismatches; if (b != b2) ++gpuPairMismatches; if (c != b) ++warpWholeVsGpu; if (c != a) ++warpWholeVsDirty;
    }
    if (dirty.frameUpdated()) ++dirtyUpdates; if (whole.frameUpdated()) ++wholeUpdates;
    if (tick >= 10) {
      dirtyCapture.add(dirty.lastCaptureMs() + dirty.lastConvertMs()); wholeCapture.add(whole.lastCaptureMs() + whole.lastConvertMs());
      if (dirty.lastReadbackMs()) dirtyReadback.add(*dirty.lastReadbackMs()); if (whole.lastReadbackMs()) wholeReadback.add(*whole.lastReadbackMs());
    }
    // Neither pool had a frame this tick: both pictures describe the same composition.
    if (tick >= 10 && !dirty.frameUpdated() && !whole.frameUpdated()) {
      ++idleComparisons;
      if (a != b) {
        ++idleMismatches;
        if (idleMismatches <= 3) {
          // Where and how much: luma bounding box, differing bytes, largest difference, luma/chroma split.
          size_t lumaBytes = 0, chromaBytes = 0; int maxDiff = 0, left = w, top = h, right = 0, bottom = 0;
          for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) {
            maxDiff = std::max(maxDiff, std::abs(int(a[i]) - int(b[i])));
            if (i < size_t(w) * h) { ++lumaBytes; const int x = int(i % size_t(w)), y = int(i / size_t(w)); left = std::min(left, x); right = std::max(right, x + 1); top = std::min(top, y); bottom = std::max(bottom, y + 1); } else ++chromaBytes;
          }
          // A third opinion on the differing box: GDI's screen read against each capture's composed BGRA.
          size_t dirtyVsGdi = 0, wholeVsGdi = 0;
          if (left < right && top < bottom) {
            const host::capture::PixelRect box{left & ~1, top & ~1, (right + 1) & ~1, (bottom + 1) & ~1};
            const size_t bytes = size_t(box.width()) * box.height() * 4; std::vector<uint8_t> gdi(bytes), fromDirty(bytes), fromWhole(bytes);
            HDC screen = GetDC(nullptr); HDC memory = CreateCompatibleDC(screen);
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = box.width(); info.bmiHeader.biHeight = -box.height(); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr; HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0); const HGDIOBJ old = SelectObject(memory, bitmap);
            BitBlt(memory, 0, 0, box.width(), box.height(), screen, box.left, box.top, SRCCOPY | CAPTUREBLT); GdiFlush(); std::memcpy(gdi.data(), bits, bytes);
            SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
            const auto grab = [&](host::Capture& capture, std::vector<uint8_t>& out) {
              capture.visitComposed(std::span<const host::capture::PixelRect>(&box, 1), [&](const uint8_t* origin, size_t pitch, const host::capture::PixelRect& piece) {
                for (int y = piece.top; y < piece.bottom; ++y) std::memcpy(out.data() + (size_t(y - box.top) * box.width() + size_t(piece.left - box.left)) * 4, origin + size_t(y - piece.top) * pitch, size_t(piece.width()) * 4);
              });
            };
            grab(dirty, fromDirty); grab(whole, fromWhole);
            for (size_t i = 0; i < bytes; i += 4) { if (std::memcmp(gdi.data() + i, fromDirty.data() + i, 3) != 0) ++dirtyVsGdi; if (std::memcmp(gdi.data() + i, fromWhole.data() + i, 3) != 0) ++wholeVsGdi; }
          }
          std::cout << "{\"idleDifference\":{\"tick\":" << tick << ",\"lumaBytes\":" << lumaBytes << ",\"chromaBytes\":" << chromaBytes << ",\"maxDiff\":" << maxDiff << ",\"lumaBox\":[" << left << ',' << top << ',' << right << ',' << bottom
                    << "],\"pixelsUnlikeGdi\":{\"dirtyWarp\":" << dirtyVsGdi << ",\"wholeGpu\":" << wholeVsGdi << "}}}" << std::endl;
        }
      }
    }
  }
  require(pairComparisons >= 20, "enough idle ticks to compare (" + std::to_string(pairComparisons) + ")");
  // The reference is a WARP capture reading whole frames: what WGC delivers to
  // a WARP device is not always what it delivers to a GPU device on the same
  // PC (measured here: a fixed 236x30 box near the taskbar differed from the
  // GPU capture and from GDI in every idle tick, identically for every WARP
  // capture, dirty or whole), and a GPU-less VM has only the WARP picture.
  require(warpWholeVsDirty == 0, "the dirty readback equals a whole-frame WARP readback at every idle tick (" + std::to_string(warpWholeVsDirty) + " of " + std::to_string(pairComparisons) + " differ)");
  require(warpPairMismatches == 0, "two dirty WARP captures agree (" + std::to_string(warpPairMismatches) + " differ)");
  if (idleMismatches) std::cout << "{\"note\":\"WARP and GPU captures of this monitor differ in " << idleMismatches << " of " << idleComparisons << " idle ticks; see idleDifference (not a property of the dirty readback)\"}\n";
  // The composed BGRA of the dirty capture (its staging texture) converts to exactly its own NV12.
  {
    std::vector<uint8_t> bgra(size_t(w) * h * 4), nv12(size_t(w) * h * 3 / 2);
    const host::capture::PixelRect all{0, 0, w, h};
    dirty.visitComposed(std::span<const host::capture::PixelRect>(&all, 1), [&](const uint8_t* origin, size_t pitch, const host::capture::PixelRect& piece) {
      for (int y = piece.top; y < piece.bottom; ++y) std::memcpy(bgra.data() + (size_t(y) * w + piece.left) * 4, origin + size_t(y - piece.top) * pitch, size_t(piece.width()) * 4);
    });
    host::capture::convertBlockToNv12(nv12, w, h, bgra.data(), size_t(w) * 4, all);
    require(nv12 == dirty.frame() || dirty.frameUpdated(), "the staging texture converts to the picture the encoder got");
  }
  std::cout << std::fixed << std::setprecision(3) << "{\"kind\":\"capture-dirty-live\",\"size\":\"" << w << 'x' << h << "\",\"dirtyBackend\":\"" << dirtyBackend << "\",\"dirtyReporting\":" << (reporting ? "true" : "false")
            << ",\"wholeBackend\":\"" << wholeBackend << "\",\"ticks\":" << ticks << ",\"dirtyUpdates\":" << dirtyUpdates << ",\"wholeUpdates\":" << wholeUpdates
            << ",\"idleComparisons\":" << idleComparisons << ",\"idleMismatches\":" << idleMismatches
            << ",\"pairComparisons\":" << pairComparisons << ",\"warpPairMismatches\":" << warpPairMismatches << ",\"gpuPairMismatches\":" << gpuPairMismatches
            << ",\"warpWholeVsGpu\":" << warpWholeVsGpu << ",\"warpWholeVsDirty\":" << warpWholeVsDirty
            << ",\"dirtyCaptureMs\":"; dirtyCapture.json(std::cout); std::cout << ",\"dirtyReadbackMs\":"; dirtyReadback.json(std::cout);
  std::cout << ",\"wholeCaptureMs\":"; wholeCapture.json(std::cout); std::cout << ",\"wholeReadbackMs\":"; wholeReadback.json(std::cout);
  std::cout << ",\"checks\":" << checks << ",\"pixelsSaved\":false}" << std::endl;
  if (!failures.empty()) { std::cerr << failures; return 1; }
  return 0;
} catch (const std::exception& error) { std::cerr << "ERROR: " << error.what() << "\n"; return 2; }
