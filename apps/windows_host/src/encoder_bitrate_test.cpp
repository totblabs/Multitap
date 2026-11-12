// Encoder-in-the-loop measurement of a LIVE CBR target change on the production
// Microsoft software H.264 MFT (host::Encoder::setBitrate), decoded by the
// client's actual RemoteDecoder. Synthetic pixels on a synthetic 60 Hz clock;
// no desktop is read. It measures how many frames a mid-stream change needs to
// take effect, that no IDR is inserted, that every frame still decodes, and the
// largest page-change frame at the new 100 Mbps ceiling versus the 4 MiB media
// packet bound. Not a real-session, relay or latency result. The MFT is pinned
// explicitly: it is the automatic fallback of the x264 production backend,
// whose live change is checked by x264_encoder_test.cpp.
#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "bitrate_controller.hpp"
#include "remote_decoder.h"
#include "../../../native/windows_video/color.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

namespace {
constexpr int fps = 60;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
uint64_t tickUs(int n) { return uint64_t(n) * 1000000 / fps; }

// Detailed motion whose cost scales smoothly with QP over the whole tested
// range: a scrolling ruled pattern over the top, a drifting gradient below,
// and fresh low-amplitude deterministic grain (+/-4) over a fixed area. High QP
// quantizes the grain away, low QP preserves it, so the CBR target (not the
// content) sets the frame size from a few Mbps to beyond 100 Mbps.
std::vector<uint8_t> page(int width, int height, int seed);
// Diagnostic-only alternative: a ClearType text page scrolling 8 px per frame
// (browser-like scrolling), the realistic counterpart of the grain scene.
bool scrollContent = false;
struct Motion {
  int width, height; std::vector<uint8_t> nv12, tall;
  Motion(int w, int h) : width(w), height(h), nv12(size_t(w) * h * 3 / 2) { if (scrollContent) tall = page(w, h * 3, 3); }
  const std::vector<uint8_t>& frame(int n) {
    if (scrollContent) {
      const int offset = (n * 8) % (2 * height), tallHeight = height * 3;
      for (int y = 0; y < height; ++y) std::memcpy(nv12.data() + size_t(y) * width, tall.data() + size_t(y + offset) * width, size_t(width));
      for (int y = 0; y < height / 2; ++y) std::memcpy(nv12.data() + size_t(width) * height + size_t(y) * width, tall.data() + size_t(width) * tallHeight + size_t(y + offset / 2) * width, size_t(width));
      return nv12;
    }
    uint32_t seed = 2463534242u ^ uint32_t(n * 2654435761u);
    for (int y = 0; y < height; ++y) {
      uint8_t* row = nv12.data() + size_t(y) * width;
      for (int x = 0; x < width; ++x) {
        const int base = y < height / 2 ? (((x + n * 5) / 6 + y / 6) % 2 ? 180 : 60) : 64 + (x + y + n * 3) % 128;
        // Grain over a fixed 1920x540 area, so its cost is the same at 4K.
        const bool grain = x < 1920 && y >= height / 2 && y < height / 2 + 540;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        row[x] = uint8_t(std::clamp(base + (grain ? int(seed % 9) - 4 : 0), 16, 235));
      }
    }
    uint8_t* chroma = nv12.data() + size_t(width) * height;
    for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width; x += 2) { chroma[size_t(y) * width + x] = uint8_t(128 + ((x + n) % 64) - 32); chroma[size_t(y) * width + x + 1] = uint8_t(128 + ((y + n) % 48) - 24); }
    return nv12;
  }
};

// Text page rendered with ClearType, like a default Windows desktop.
std::vector<uint8_t> page(int width, int height, int seed) {
  HDC dc = CreateCompatibleDC(nullptr); BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  uint8_t* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
  require(dc && bitmap && pixels, "Synthetic page canvas");
  const auto previous = SelectObject(dc, bitmap); RECT all{0, 0, width, height}; FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  HFONT fonts[] = {CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI"),
                   CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic"),
                   CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI")};
  const wchar_t* words[] = {L"desktop", L"report", L"2026-10-25", L"session", L"quality", L"scroll", L"문서", L"합계", L"지도", L"1,234,500", L"relay", L"bitrate"};
  int y = 8;
  for (int line = 0; y < height - 20; ++line) {
    SelectObject(dc, fonts[(line + seed) % 3]); SetTextColor(dc, RGB(20 + 30 * ((line + seed) % 3), 20, 30 + 40 * (seed % 2)));
    std::wstring text = std::to_wstring(line + 1) + L"  ";
    for (int k = 0; k < width / 60; ++k) text += std::wstring(words[(line * 7 + k * 3 + seed * 5) % 12]) + L" ";
    TextOutW(dc, 16, y, text.c_str(), int(text.size())); y += 14 + 4 * ((line + seed) % 3);
  }
  GdiFlush();
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2); host::bgraToNv12(pixels, nv12.data(), width, height);
  SelectObject(dc, previous); for (auto font : fonts) DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
  return nv12;
}
double psnr(int width, int height, const std::vector<uint8_t>& source, const std::vector<uint8_t>& decoded) {
  std::vector<uint8_t> reference(size_t(width) * height * 4);
  multitap::video::nv12ToRgba(source.data(), reference.data(), width, height, width, height, false, false);
  double sum = 0; for (size_t i = 0; i < reference.size(); ++i) if (i % 4 != 3) { const double e = double(reference[i]) - decoded[i]; sum += e * e; }
  const double mse = sum / (double(width) * height * 3); return mse < 1e-9 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

struct Phase { int bitrate, frames; };
struct PhaseResult { int bitrate = 0, settleFrames = -1; double meanMbps = 0, firstFrameRatio = 0, psnrDb = 0; size_t keys = 0, maxBytes = 0; std::vector<double> early; };

// One encoder for the whole run; every phase after the first is a live change.
std::vector<PhaseResult> liveChanges(int width, int height, const std::vector<Phase>& phases, size_t& decodedFrames, size_t& encodedFrames) {
  Motion motion(width, height);
  host::Encoder encoder(width, height, fps, phases.front().bitrate, {}, host::EncoderBackend::mft);
  require(encoder.reportedBitrate() == 0 || encoder.reportedBitrate() == phases.front().bitrate, "Initial MFT mean bitrate differs from the requested rate");
  multitap::RemoteDecoder decoder; host::check(decoder.Initialize(width, height), "Initialize actual client decoder");
  std::map<uint64_t, size_t> sizes; std::map<uint64_t, bool> keys; std::map<uint64_t, std::vector<uint8_t>> sources; std::map<uint64_t, double> quality;
  decodedFrames = encodedFrames = 0;
  const auto output = [&](host::Frame frame) {
    ++encodedFrames; sizes[frame.ptsUs] = frame.bytes.size(); keys[frame.ptsUs] = frame.key;
    host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& shown) {
      require(shown.width == UINT(width) && shown.height == UINT(height), "Decoded geometry changed");
      ++decodedFrames;
      if (const auto source = sources.find(shown.pts_us); source != sources.end()) { quality[shown.pts_us] = psnr(width, height, source->second, shown.rgba); sources.erase(source); }
    }), "Actual client decode after a live bitrate change");
  };
  std::vector<std::pair<int, int>> spans; int n = 0;
  for (size_t p = 0; p < phases.size(); ++p) {
    if (p) {
      require(encoder.setBitrate(phases[p].bitrate), "The MFT refused a live mean-bitrate change");
      const auto reported = encoder.reportedBitrate();
      require(reported == 0 || reported == phases[p].bitrate, "MFT reports a different mean bitrate after the change");
    }
    spans.push_back({n, n + phases[p].frames});
    for (int k = 0; k < phases[p].frames; ++k, ++n) {
      const auto& frame = motion.frame(n);
      if (k >= phases[p].frames - 9 && k < phases[p].frames - 1) sources[tickUs(n)] = frame;  // score the end of the phase
      encoder.encode(frame.data(), tickUs(n), output);
    }
  }
  encoder.finish(output);
  host::check(decoder.Drain([&](multitap::DecodedFrame&& shown) { ++decodedFrames; if (const auto source = sources.find(shown.pts_us); source != sources.end()) { quality[shown.pts_us] = psnr(width, height, source->second, shown.rgba); sources.erase(source); } }), "Drain client decoder");
  std::vector<PhaseResult> results;
  for (size_t p = 0; p < phases.size(); ++p) {
    PhaseResult r; r.bitrate = phases[p].bitrate; const double budget = double(r.bitrate) / 8 / fps;
    std::vector<size_t> frameSizes;
    for (int k = spans[p].first; k < spans[p].second; ++k) { const auto found = sizes.find(tickUs(k)); frameSizes.push_back(found == sizes.end() ? 0 : found->second); if (keys[tickUs(k)]) ++r.keys; }
    // Settled once a trailing 10-frame window is within +/-20 % of the budget.
    for (size_t k = 0; k + 10 <= frameSizes.size(); ++k) { double sum = 0; for (size_t j = k; j < k + 10; ++j) sum += double(frameSizes[j]); if (std::abs(sum / 10 - budget) <= 0.2 * budget) { r.settleFrames = int(k); break; } }
    double total = 0; for (size_t k = frameSizes.size() / 2; k < frameSizes.size(); ++k) total += double(frameSizes[k]);
    r.meanMbps = total * 8 * fps / double(frameSizes.size() - frameSizes.size() / 2) / 1e6;
    r.firstFrameRatio = frameSizes.empty() ? 0 : double(frameSizes.front()) / budget;
    for (size_t k = 0; k < std::min<size_t>(12, frameSizes.size()); ++k) r.early.push_back(double(frameSizes[k]) / budget);
    for (const auto bytes : frameSizes) r.maxBytes = std::max(r.maxBytes, bytes);
    int scored = 0; for (const auto& [pts, value] : quality) if (pts >= tickUs(spans[p].first) && pts < tickUs(spans[p].second)) { r.psnrDb += value; ++scored; }
    if (scored) r.psnrDb /= scored;
    results.push_back(r);
  }
  return results;
}

// Production change-only policy with refinement: an idle text page, then a
// whole-page switch. Reports the largest frame against the 4 MiB packet bound.
size_t pageChangeMaxFrame(int width, int height, int bitrate) {
  const auto a = page(width, height, 1), b = page(width, height, 2);
  host::Encoder encoder(width, height, fps, bitrate, {}, host::EncoderBackend::mft);
  host::FrameEncodePolicy policy(size_t(width) * height * 3 / 2, host::FrameRefinement::forCbr(bitrate, fps));
  size_t largest = 0;
  for (int n = 0; n < 240; ++n) {
    const auto& frame = n < 150 ? a : b;
    const auto decision = policy.prepare(frame, tickUs(n)); if (!decision.submit) continue;
    encoder.encode(frame.data(), tickUs(n), [&](host::Frame output) {
      require(policy.output(output.ptsUs, output.key, tickUs(n), output.bytes.size()), "Page-change output rejected");
      if (output.ptsUs >= tickUs(150)) largest = std::max(largest, output.bytes.size());
    });
  }
  return largest;
}

void print(const PhaseResult& r) {
  std::cout << "{\"targetMbps\":" << r.bitrate / 1e6 << ",\"settleFrames\":" << r.settleFrames << ",\"secondHalfMbps\":" << r.meanMbps << ",\"firstFrameBudgetRatio\":" << r.firstFrameRatio
            << ",\"maxFrameBytes\":" << r.maxBytes << ",\"keyFrames\":" << r.keys << ",\"psnrDb\":" << r.psnrDb << ",\"first12BudgetRatios\":[";
  for (size_t k = 0; k < r.early.size(); ++k) std::cout << (k ? "," : "") << r.early[k];
  std::cout << "]}";
}
}

int main(int argc, char** argv) {
  try {
    host::Runtime runtime;
    std::cout << std::fixed << std::setprecision(2);
    // Diagnostic: one fixed-rate run, e.g. --phases 3840 2160 20,8,60 (Mbps).
    if ((argc == 5 || argc == 6) && std::string(argv[1]) == "--phases") {
      scrollContent = argc == 6 && std::string(argv[5]) == "scroll";
      std::vector<Phase> phases; std::string list = argv[4];
      for (size_t at = 0; at < list.size();) { const auto end = list.find(',', at); phases.push_back({std::stoi(list.substr(at, end - at)) * 1000000, 90}); if (end == std::string::npos) break; at = end + 1; }
      size_t decoded = 0, encoded = 0; const auto results = liveChanges(std::stoi(argv[2]), std::stoi(argv[3]), phases, decoded, encoded);
      std::cout << "{\"encoded\":" << encoded << ",\"decoded\":" << decoded << ",\"phases\":["; for (size_t p = 0; p < results.size(); ++p) { if (p) std::cout << ','; print(results[p]); } std::cout << "]}\n";
      return 0;
    }
    bool ok = true;
    const auto report = [](const char* name, int width, int height, const std::vector<PhaseResult>& results, size_t encoded, size_t decoded) {
      size_t keys = 0; for (const auto& r : results) keys += r.keys;
      std::cout << "{\"kind\":\"encoder-live-bitrate\",\"case\":\"" << name << "\",\"width\":" << width << ",\"height\":" << height << ",\"encoded\":" << encoded << ",\"decoded\":" << decoded << ",\"keyFrames\":" << keys << ",\"phases\":[";
      for (size_t p = 0; p < results.size(); ++p) { if (p) std::cout << ','; print(results[p]); }
      std::cout << "]}\n";
      require(encoded == decoded && encoded == 90 * results.size(), "A live bitrate change dropped or broke frames");
      require(keys == 1 && results.front().keys == 1, "A live bitrate change inserted an IDR");
    };
    const auto within = [](const PhaseResult& r, double tolerance) { return std::abs(r.meanMbps - r.bitrate / 1e6) <= tolerance * r.bitrate / 1e6; };
    {
      // 1080p60 detailed motion: every phase after the first is a live change.
      size_t decoded = 0, encoded = 0;
      const auto results = liveChanges(1920, 1080, {{20000000, 90}, {8000000, 90}, {60000000, 90}, {100000000, 90}, {12000000, 90}}, decoded, encoded);
      report("1080p-grain", 1920, 1080, results, encoded, decoded);
      // Settled inside 10 frames and the second half within 25 % of the target.
      for (size_t p = 1; p < results.size(); ++p) if (results[p].settleFrames < 0 || results[p].settleFrames > 10 || !within(results[p], 0.25)) ok = false;
      require(results[2].psnrDb > results[1].psnrDb + 3 && results[3].psnrDb >= results[0].psnrDb, "Quality did not follow the live bitrate");
    }
    const int floor4k = host::adaptiveBitrateFloor(3840, 2160, fps);
    {
      // 4K60 detailed motion down to the adaptive floor and back up to 100 Mbps.
      size_t decoded = 0, encoded = 0;
      const auto results = liveChanges(3840, 2160, {{60000000, 90}, {floor4k, 90}, {100000000, 90}, {30000000, 90}}, decoded, encoded);
      report("4k-grain-floor", 3840, 2160, results, encoded, decoded);
      if (!within(results[2], 0.25) || results[2].psnrDb < 30 || results[1].psnrDb < 18) ok = false;
    }
    {
      // 4K60 scrolling ClearType text: realistic content, floor and back.
      scrollContent = true; size_t decoded = 0, encoded = 0;
      const auto results = liveChanges(3840, 2160, {{20000000, 90}, {floor4k, 90}, {60000000, 90}}, decoded, encoded);
      scrollContent = false;
      report("4k-text-scroll-floor", 3840, 2160, results, encoded, decoded);
      if (results[1].psnrDb < 28 || results[2].psnrDb < 35) ok = false;
    }
    {
      // Diagnostic only (Windows-build dependent, not asserted): a 4K encoder
      // started far below the floor stays broken even after a raise to 100 M.
      scrollContent = true; size_t decoded = 0, encoded = 0;
      const auto results = liveChanges(3840, 2160, {{8000000, 90}, {100000000, 90}}, decoded, encoded);
      scrollContent = false;
      std::cout << "{\"kind\":\"encoder-below-floor-diagnostic\",\"width\":3840,\"height\":2160,\"startMbps\":8,\"floorMbps\":" << floor4k / 1e6 << ",\"phases\":["; print(results[0]); std::cout << ','; print(results[1]); std::cout << "]}\n";
    }
    // Whole-page text change after an idle page. The ceiling raise must not
    // make the change frame larger. Measured: the MFT codes it at
    // the same size at 20 and 100 Mbps; 1080p ~1.2 MB, but 4K ~4.9 MB, above
    // the 4 MiB media packet bound (a PRE-EXISTING failure at the released 20
    // Mbps too, reported, not asserted here; CODECAPI_AVEncVideoMinQP 20
    // brought it to 4.06 MB in a probe, buffer size had no effect).
    for (const auto [width, height] : {std::pair{1920, 1080}, std::pair{3840, 2160}}) {
      size_t sizes[2]{};
      for (int n = 0; n < 2; ++n) {
        const int bitrate = n ? 100000000 : 20000000; sizes[n] = pageChangeMaxFrame(width, height, bitrate);
        std::cout << "{\"kind\":\"page-change-frame\",\"width\":" << width << ",\"height\":" << height << ",\"targetMbps\":" << bitrate / 1e6 << ",\"maxFrameBytes\":" << sizes[n] << ",\"mediaPacketLimit\":" << 4 * 1024 * 1024 - 12 << ",\"withinLimit\":" << (sizes[n] + 12 <= 4 * 1024 * 1024 ? "true" : "false") << "}\n";
      }
      if (sizes[1] > sizes[0] + sizes[0] / 10 || (width == 1920 && sizes[1] + 12 > 4 * 1024 * 1024)) ok = false;
    }
    std::cout << "{\"kind\":\"encoder-live-bitrate-summary\",\"passed\":" << (ok ? "true" : "false") << ",\"actualMicrosoftCpuEncoder\":true,\"actualClientDecoder\":true,\"syntheticPixels\":true,\"desktopCaptured\":false,\"relayUsed\":false,\"realSessionAcceptance\":false}\n";
    return ok ? 0 : 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
