// Encoder-in-the-loop check of FrameEncodePolicy refinement: the production
// encoder (host::Encoder: x264 when built in, else the software H.264 MFT; the
// MFT is also checked explicitly as the fallback) and the client's RemoteDecoder, driven on a
// synthetic 60 Hz clock with generated text pages (no desktop is read). It
// verifies bounded behaviour and that refinement actually sharpens decoded text
// versus the original policy. Not a real-session, relay or latency result.
#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "remote_decoder.h"
#include "../../../native/windows_video/color.hpp"
#include <cmath>
#include <iostream>
#include <map>
#include <string>

namespace {
constexpr int width = 1920, height = 1080, fps = 60, bitrate = 12000000;
constexpr size_t frameBytes = size_t(width) * height * 3 / 2;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
uint64_t tickUs(int n) { return uint64_t(n) * 1000000 / fps; }

// Text page rendered with ClearType like a default Windows desktop.
std::vector<uint8_t> page(int seed) {
  HDC dc = CreateCompatibleDC(nullptr); BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  uint8_t* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
  require(dc && bitmap && pixels, "Synthetic page canvas");
  const auto previous = SelectObject(dc, bitmap); RECT all{0, 0, width, height}; FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  HFONT fonts[] = {CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI"),
                   CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic"),
                   CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI")};
  const wchar_t* words[] = {L"원격", L"desktop", L"선명도", L"report", L"2026-10-25", L"합계", L"₩1,234,500", L"session", L"문서", L"quality", L"지도", L"scroll"};
  int y = 8;
  for (int line = 0; y < height - 20; ++line) {
    const auto font = fonts[(line + seed) % 3]; SelectObject(dc, font); SetTextColor(dc, RGB(20 + 30 * ((line + seed) % 3), 20, 30));
    std::wstring text = std::to_wstring(line + 1) + L"  ";
    for (int k = 0; k < 24; ++k) text += std::wstring(words[(line * 7 + k * 3 + seed * 5) % 12]) + L" ";
    TextOutW(dc, 16, y, text.c_str(), int(text.size())); y += 14 + 4 * ((line + seed) % 3);
  }
  GdiFlush();
  std::vector<uint8_t> nv12(frameBytes); host::bgraToNv12(pixels, nv12.data(), width, height);
  SelectObject(dc, previous); for (auto font : fonts) DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
  return nv12;
}
double psnr(const std::vector<uint8_t>& source, const std::vector<uint8_t>& decoded) {
  std::vector<uint8_t> reference(size_t(width) * height * 4);
  multitap::video::nv12ToRgba(source.data(), reference.data(), width, height, width, height, false, false);
  double sum = 0; for (size_t i = 0; i < reference.size(); ++i) if (i % 4 != 3) { const double e = double(reference[i]) - decoded[i]; sum += e * e; }
  const double mse = sum / (double(width) * height * 3); return mse < 1e-9 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

struct Run {
  Run(host::FrameRefinement refinement, host::EncoderBackend backend) : encoder(width, height, fps, bitrate, {}, backend), policy(frameBytes, refinement) { host::check(decoder.Initialize(width, height), "Decoder"); }
  host::Encoder encoder; host::FrameEncodePolicy policy; multitap::RemoteDecoder decoder;
  size_t submits = 0, refines = 0, bytes = 0, maxBytes = 0, keys = 0; std::map<uint64_t, double> quality; std::vector<uint64_t> submitted, refined;
  const std::vector<uint8_t>* source = nullptr; bool measure = false;
  void tick(const std::vector<uint8_t>& frame, int n, bool score) {
    const auto decision = policy.prepare(frame, tickUs(n)); if (!decision.submit) return;
    ++submits; refines += decision.refine; submitted.push_back(tickUs(n)); if (decision.refine) refined.push_back(tickUs(n)); source = &frame; measure = score;
    encoder.encode(frame.data(), tickUs(n), [&](host::Frame output) {
      require(policy.output(output.ptsUs, output.key, tickUs(n), output.bytes.size()), "Actual encoder output rejected");
      bytes += output.bytes.size(); maxBytes = std::max(maxBytes, output.bytes.size()); keys += output.key;
      host::check(decoder.Decode(output.bytes.data(), output.bytes.size(), output.ptsUs, [&](multitap::DecodedFrame&& f) {
        require(f.width == UINT(width) && f.height == UINT(height), "Decoded size"); if (measure) quality[f.pts_us] = psnr(*source, f.rgba);
      }), "Actual client decode");
    });
  }
  double last(uint64_t from, uint64_t to) const { double q = 0; for (const auto& [pts, value] : quality) if (pts >= from && pts < to) q = value; return q; }
  double first(uint64_t from, uint64_t to) const { for (const auto& [pts, value] : quality) if (pts >= from && pts < to) return value; return 0; }
  size_t submitsIn(uint64_t from, uint64_t to) const { size_t n = 0; for (const auto pts : submitted) n += pts >= from && pts < to; return n; }
  size_t refinesIn(uint64_t from, uint64_t to) const { size_t n = 0; for (const auto pts : refined) n += pts >= from && pts < to; return n; }
};

// The production backend (x264 when built in) and the explicit MFT (its
// fallback) go through the same scenario and the same bounds.
void scenario(host::EncoderBackend backend) {
    const auto a = page(1), b = page(2);
    Run plain{host::FrameRefinement{}, backend}, refined{host::FrameRefinement::forCbr(bitrate, fps), backend};
    require(plain.encoder.backend() == refined.encoder.backend(), "Backends differ");
    std::vector<uint8_t> frame;
    // Phase 1, ticks 0-119: start-up IDR on a text page, then static.
    // Phase 2, ticks 120-239: whole-page switch, then static.
    // Phase 3, ticks 240-419: cursor-sized changes every 300 ms.
    // Phase 4, ticks 420-479: continuous motion (a band moving every tick); 480-539 static.
    size_t phase2Bytes = 0;
    for (int n = 0; n < 540; ++n) {
      if (n < 120) frame = a;
      else if (n < 240) frame = b;
      else if (n < 420) { frame = b; const int k = (n - 240) / 18; for (int y = 0; y < 16; ++y) std::fill_n(frame.begin() + std::ptrdiff_t(size_t(400 + y) * width + 300 + 37 * k), 16, uint8_t(16)); }
      else if (n < 480) { frame = b; for (int y = 0; y < 64; ++y) std::fill_n(frame.begin() + std::ptrdiff_t(size_t(500 + y) * width + 8 * (n - 420)), 400, uint8_t(16 + (n * 7) % 200)); }
      else { frame = b; for (int y = 0; y < 64; ++y) std::fill_n(frame.begin() + std::ptrdiff_t(size_t(500 + y) * width + 8 * 59), 400, uint8_t(16 + (479 * 7) % 200)); }
      const bool score = n < 240;
      const size_t before = refined.bytes;
      plain.tick(frame, n, score); refined.tick(frame, n, score);
      if (n >= 120 && n < 240) phase2Bytes += refined.bytes - before;
      if (n == 239) require(!refined.policy.refinementArmed(), "Refinement was still running 2 s after a page switch");
    }
    const double plainStart = plain.last(0, tickUs(120)), refinedStartFirst = refined.first(0, tickUs(120)), refinedStart = refined.last(0, tickUs(120));
    const double plainSwitch = plain.last(tickUs(120), tickUs(240)), refinedSwitch = refined.last(tickUs(120), tickUs(240));
    std::cout << "{\"encoder\":\"" << refined.encoder.backendName() << "\",\"startPsnr\":{\"original\":" << plainStart << ",\"refinedFirst\":" << refinedStartFirst << ",\"refined\":" << refinedStart << "},\"switchPsnr\":{\"original\":" << plainSwitch << ",\"refined\":" << refinedSwitch
              << "},\"submits\":{\"original\":" << plain.submits << ",\"refined\":" << refined.submits << ",\"refinements\":" << refined.refines << ",\"refinementsPerPhase\":[" << refined.refinesIn(0, tickUs(120)) << ',' << refined.refinesIn(tickUs(120), tickUs(240)) << ',' << refined.refinesIn(tickUs(240), tickUs(480)) << ',' << refined.refinesIn(tickUs(480), tickUs(540)) << "]},\"phase2Bytes\":" << phase2Bytes << ",\"maxFrameBytes\":{\"original\":" << plain.maxBytes << ",\"refined\":" << refined.maxBytes << "}}\n";
    require(plain.refines == 0 && plain.keys == 1 && refined.keys == 1, "Refinement produced key frames or the original policy refined");
    // Each armed change is bounded by maximumFrames (90). The start-up IDR and
    // the page switch arm; cursor-sized changes and continuous motion never
    // refine; the moving band's final frame may arm once more (x264 codes it
    // above the trigger at its quality cap), bounded the same way.
    require(refined.refinesIn(0, tickUs(120)) > 0 && refined.refinesIn(0, tickUs(120)) <= 90 && refined.refinesIn(tickUs(120), tickUs(240)) > 0 && refined.refinesIn(tickUs(120), tickUs(240)) <= 90, "Refinement did not run or exceeded its bound for an armed change");
    require(refined.refinesIn(tickUs(240), tickUs(480)) == 0 && refined.refinesIn(tickUs(480), tickUs(540)) <= 90, "Refinement ran during cursor-sized changes or motion, or exceeded its bound");
    require(refinedStart >= refinedStartFirst + 3 && refinedStart >= plainStart + 3, "Refinement did not sharpen the start-up IDR");
    require(refinedSwitch >= plainSwitch + 6 && refinedSwitch >= 30, "Refinement did not sharpen the page switch");
    require(phase2Bytes <= size_t(bitrate) / 8 * 2, "Page-switch refinement exceeded the configured bitrate over its 2 s window");
    // Quiescent after each episode: only the 1 s refresh remains once the
    // episode has ended, at the latest after the 100 ms quiet period (6 ticks)
    // plus maximumFrames (90). The MFT settles earlier; x264 at its CRF cap and
    // 12 Mbps keeps adding detail (~54 dB) up to that bound on a full text page.
    const int episodeEnd = 6 + int(host::FrameRefinement::forCbr(bitrate, fps).maximumFrames) + 1;
    require(refined.submitsIn(tickUs(episodeEnd), tickUs(120)) <= 1 && refined.submitsIn(tickUs(120 + episodeEnd), tickUs(240)) <= 1, "Settled refinement kept submitting frames");
    // Cursor-sized changes and continuous motion: identical submissions to the original policy.
    require(refined.submitsIn(tickUs(240), tickUs(420)) == plain.submitsIn(tickUs(240), tickUs(420)), "Cursor-sized changes triggered refinement");
    require(refined.submitsIn(tickUs(420), tickUs(480)) == 60 && plain.submitsIn(tickUs(420), tickUs(480)) == 60, "Continuous motion submissions changed");
}
}

int main() {
  try {
    host::Runtime runtime;
    scenario(host::EncoderBackend::automatic);
    if (host::x264EncoderAvailable()) scenario(host::EncoderBackend::mft);
    std::cout << "{\"kind\":\"frame-refinement-encoder-loop\",\"production\":\"" << (host::x264EncoderAvailable() ? "x264" : "mft") << "\",\"mftChecked\":true,\"actualClientDecoder\":true,\"syntheticPixels\":true,\"desktopCaptured\":false,\"realSessionAcceptance\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
