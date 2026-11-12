#include "owned_output_size.hpp"
// Short real Windows Graphics Capture A/B probe on this PC's primary monitor:
// one Capture on the new direct path, one forced onto the previous DIB path.
// Frames stay in memory; only timings, counters and 64-bit digests are
// printed. No pixels are saved or transmitted; no display setting changes.
#include "capture_selection.hpp"
#include <iomanip>
#include <iostream>
#include <numeric>

namespace {
uint64_t digest(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 0x9e3779b97f4a7c15ull ^ bytes.size(); size_t at = 0;
  for (; at + 8 <= bytes.size(); at += 8) { uint64_t word; std::memcpy(&word, bytes.data() + at, 8); hash = (hash ^ word) * 0x100000001b3ull; hash ^= hash >> 29; }
  for (; at < bytes.size(); ++at) hash = (hash ^ bytes[at]) * 0x100000001b3ull;
  return hash;
}
struct Stats { std::vector<double> v; void add(double x) { v.push_back(x); } double mean() const { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()); }
  double p95() const { if (v.empty()) return 0; auto s = v; std::sort(s.begin(), s.end()); return s[std::min(s.size() - 1, size_t(0.95 * double(s.size() - 1) + 0.5))]; } };
}
int wmain(int argc, wchar_t** argv) {
  try {
    const int iterations = argc > 1 ? std::clamp(std::stoi(argv[1]), 10, 600) : 180;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // Same as main.cpp; this process only.
    host::Runtime runtime;
    if (!host::interactiveDesktopAvailable()) { std::cout << "{\"kind\":\"capture-path-probe\",\"state\":\"no_interactive_desktop\"}\n"; return 3; }
    const auto monitors = host::currentCaptureMonitors();
    auto chosen = std::find_if(monitors.begin(), monitors.end(), [](const auto& m) { return m.primary; }); if (chosen == monitors.end()) chosen = monitors.begin();
    const RECT source = chosen->bounds; const int width = int(source.right - source.left), height = int(source.bottom - source.top);
    if (!ownedOutputSize(uint64_t(std::max(width,0)),uint64_t(std::max(height,0)))) { std::cout << "{\"kind\":\"capture-path-probe\",\"state\":\"unsupported_monitor_size\"}\n"; return 3; }
    if (argc > 2 && (std::wstring(argv[2]) == L"--scaled" || std::wstring(argv[2]) == L"--direct")) {
      // The product's scaled WGC path (encoder 3/4 of the monitor): resample
      // only new frames, put back the pixels under last tick's cursor.
      // --direct: the same report for the unscaled direct path (comparison).
      const bool direct = std::wstring(argv[2]) == L"--direct";
      const int scaledWidth = direct ? width : (width * 3 / 4) & ~1, scaledHeight = direct ? height : (height * 3 / 4) & ~1;
      host::Capture scaled(scaledWidth, scaledHeight, source);
      Stats wall, fresh, quiet, freshCapture, freshConvert; int updates = 0, compared = 0, stable = 0; uint64_t previous = 0; bool previousQuiet = false;
      for (int i = 0; i < iterations; ++i) {
        const auto began = host::Clock::now(); const auto& frame = scaled.frame(); const double ms = std::chrono::duration<double, std::milli>(host::Clock::now() - began).count();
        const uint64_t d = digest(frame); const bool updated = scaled.frameUpdated(); updates += updated;
        if (i >= 10) { wall.add(ms); (updated ? fresh : quiet).add(ms); if (updated) { freshCapture.add(scaled.lastCaptureMs()); freshConvert.add(scaled.lastConvertMs()); } }  // the first frames wait for WGC's initial frame
        CURSORINFO cursor{sizeof(cursor)}; GetCursorInfo(&cursor); static POINT last{}; const bool still = cursor.ptScreenPos.x == last.x && cursor.ptScreenPos.y == last.y; last = cursor.ptScreenPos;
        if (i > 2 && !updated && still && previousQuiet) { ++compared; stable += d == previous; }
        previous = d; previousQuiet = !updated && still;
        Sleep(16);
      }
      std::cout << std::fixed << std::setprecision(3) << "{\"kind\":\"capture-path-probe\",\"mode\":\"" << (direct ? "direct" : "scaled") << "\",\"state\":\"ok\",\"freshCaptureMeanMs\":" << freshCapture.mean() << ",\"freshConvertMeanMs\":" << freshConvert.mean() << ",\"source\":\"" << width << 'x' << height << "\",\"encoded\":\"" << scaledWidth << 'x' << scaledHeight << "\",\"backend\":\"" << scaled.backend()
                << "\",\"iterations\":" << iterations << ",\"freshFrames\":" << updates << ",\"frameWallMeanMs\":" << wall.mean() << ",\"frameWallP95Ms\":" << wall.p95() << ",\"freshMeanMs\":" << fresh.mean() << ",\"freshP95Ms\":" << fresh.p95() << ",\"quietMeanMs\":" << quiet.mean() << ",\"quietP95Ms\":" << quiet.p95()
                << ",\"quietIterationsCompared\":" << compared << ",\"quietIterationsStable\":" << stable << ",\"pixelsSaved\":false,\"network\":false,\"displaySettingsChanged\":false}\n";
      return compared == stable ? 0 : 2;
    }
    host::captureProbeLegacyPath = false; host::Capture direct(width, height, source);
    host::captureProbeLegacyPath = true; host::Capture legacy(width, height, source);
    host::captureProbeLegacyPath = false;
    Stats directWall, legacyWall, directCapture, directConvert, legacyCapture, legacyConvert;
    int compared = 0, equal = 0, directUpdates = 0, legacyUpdates = 0; uint64_t previousDirect = 0, previousLegacy = 0;
    for (int i = 0; i < iterations; ++i) {
      // Alternate the call order so neither path systematically sees newer input.
      const bool directFirst = i % 2 == 0; uint64_t d = 0, l = 0; bool du = false, lu = false;
      for (int k = 0; k < 2; ++k) {
        const bool runDirect = (k == 0) == directFirst; auto& capture = runDirect ? direct : legacy;
        const auto began = host::Clock::now(); const auto& frame = capture.frame(); const double ms = std::chrono::duration<double, std::milli>(host::Clock::now() - began).count();
        (runDirect ? directWall : legacyWall).add(ms); (runDirect ? directCapture : legacyCapture).add(capture.lastCaptureMs()); (runDirect ? directConvert : legacyConvert).add(capture.lastConvertMs());
        (runDirect ? d : l) = digest(frame); (runDirect ? du : lu) = capture.frameUpdated();
      }
      directUpdates += du; legacyUpdates += lu;
      // Compare only quiet iterations: neither path saw a new surface now, and
      // each path's own output was unchanged since the previous iteration.
      if (i > 2 && !du && !lu && d == previousDirect && l == previousLegacy) { ++compared; equal += d == l; }
      previousDirect = d; previousLegacy = l;
      Sleep(16);
    }
    std::cout << std::fixed << std::setprecision(3) << "{\"kind\":\"capture-path-probe\",\"state\":\"ok\",\"width\":" << width << ",\"height\":" << height << ",\"iterations\":" << iterations
              << ",\"directBackend\":\"" << direct.backend() << "\",\"legacyBackend\":\"" << legacy.backend() << "\",\"directFreshFrames\":" << directUpdates << ",\"legacyFreshFrames\":" << legacyUpdates
              << ",\"quietIterationsCompared\":" << compared << ",\"quietIterationsEqual\":" << equal
              << ",\"direct\":{\"frameWallMeanMs\":" << directWall.mean() << ",\"frameWallP95Ms\":" << directWall.p95() << ",\"captureMeanMs\":" << directCapture.mean() << ",\"convertMeanMs\":" << directConvert.mean() << '}'
              << ",\"legacy\":{\"frameWallMeanMs\":" << legacyWall.mean() << ",\"frameWallP95Ms\":" << legacyWall.p95() << ",\"captureMeanMs\":" << legacyCapture.mean() << ",\"convertMeanMs\":" << legacyConvert.mean() << '}'
              << ",\"pixelsSaved\":false,\"network\":false,\"displaySettingsChanged\":false,\"note\":\"dev PC desktop, mostly static; not a GPUless VM149 measurement\"}\n";
    return compared > 0 && equal == compared ? 0 : 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
