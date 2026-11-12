// Encoder-in-the-loop check of ToTBSeamless lossless tiles: the production
// software H.264 MFT (host::Encoder) with the production refinement policy, the
// host LosslessTileSource, and the client's RemoteDecoder + lossless::Compositor,
// on a synthetic 60 Hz clock with GDI-rendered desktop/text pages, a cursor
// sprite, scrolling, map panning and typing. No desktop is captured and no relay,
// network or real session is involved: this is not a real-session result.
#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "lossless_tile_source.hpp"
#include "remote_decoder.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace {
constexpr int width = 1920, height = 1080, fps = 60, bitrate = 12000000;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
uint64_t tickUs(int n) { return uint64_t(n + 1) * 1000000 / fps; }
using Clock = std::chrono::steady_clock;
double since(Clock::time_point began) { return std::chrono::duration<double, std::milli>(Clock::now() - began).count(); }

struct Dib {
  HDC dc = CreateCompatibleDC(nullptr); HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr; uint8_t* pixels = nullptr;
  Dib() {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(dc && bitmap && pixels, "Synthetic canvas"); previous = SelectObject(dc, bitmap); SetBkMode(dc, TRANSPARENT);
  }
  ~Dib() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
  std::vector<uint8_t> bytes() { GdiFlush(); std::vector<uint8_t> value(pixels, pixels + size_t(width) * height * 4); for (size_t i = 3; i < value.size(); i += 4) value[i] = 255; return value; }
};
HFONT font(int size, int weight, const wchar_t* face) { return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face); }

// A Windows-like desktop: blue wallpaper gradient, colourful system icons with
// ClearType labels (including the user's "네이버 웨일" shortcut case).
std::vector<uint8_t> desktopPage() {
  Dib dib;
  for (int y = 0; y < height; ++y) { HBRUSH brush = CreateSolidBrush(RGB(0, 70 + y * 60 / height, 140 + y * 80 / height)); RECT row{0, y, width, y + 1}; FillRect(dib.dc, &row, brush); DeleteObject(brush); }
  HFONT label = font(12, FW_NORMAL, L"Segoe UI"), korean = font(12, FW_NORMAL, L"Malgun Gothic");
  const wchar_t* names[] = {L"네이버 웨일", L"휴지통", L"내 PC", L"문서", L"Microsoft Edge", L"제어판", L"보고서.xlsx", L"지도"};
  LPCWSTR icons[] = {IDI_APPLICATION, IDI_INFORMATION, IDI_WARNING, IDI_ERROR, IDI_SHIELD, IDI_QUESTION, IDI_WINLOGO, IDI_ASTERISK};
  for (int n = 0; n < 24; ++n) {
    const int x = 24 + (n / 8) * 96, y = 20 + (n % 8) * 110;
    DrawIconEx(dib.dc, x + 20, y, LoadIconW(nullptr, icons[n % 8]), 48, 48, 0, nullptr, DI_NORMAL);
    // A small colourful "app" badge next to some icons.
    if (n % 3 == 0) for (int r = 14; r > 0; r -= 2) { HBRUSH brush = CreateSolidBrush(RGB(255 - r * 9, 40 + r * 12, 60 + (n * 37 + r * 20) % 190)); auto old = SelectObject(dib.dc, brush); Ellipse(dib.dc, x + 58 - r, y + 30 - r, x + 58 + r, y + 30 + r); SelectObject(dib.dc, old); DeleteObject(brush); }
    SelectObject(dib.dc, n % 2 ? label : korean); SetTextColor(dib.dc, RGB(255, 255, 255));
    RECT text{x - 8, y + 54, x + 96, y + 90}; DrawTextW(dib.dc, names[n % 8], -1, &text, DT_CENTER | DT_WORDBREAK);
  }
  // A light window with dense colourful text.
  RECT window{560, 120, 1840, 980}; FillRect(dib.dc, &window, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  HFONT body = font(14, FW_NORMAL, L"Segoe UI");
  for (int line = 0; line < 44; ++line) {
    SelectObject(dib.dc, line % 4 ? body : korean); SetTextColor(dib.dc, RGB(20 + (line * 47) % 200, 30, 60 + (line * 23) % 160));
    std::wstring text = std::to_wstring(line + 1) + L"  네이버 웨일 브라우저 · ToTBSeamless 원격 데스크톱 선명도 비교 · report 2026-10-25 ₩1,234,500";
    TextOutW(dib.dc, 580, 132 + line * 19, text.c_str(), int(text.size()));
  }
  DeleteObject(label); DeleteObject(korean); DeleteObject(body);
  return dib.bytes();
}
std::vector<uint8_t> documentPage(int seed) {
  Dib dib; RECT all{0, 0, width, height}; FillRect(dib.dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  HFONT fonts[] = {font(12, FW_NORMAL, L"Segoe UI"), font(14, FW_NORMAL, L"Malgun Gothic"), font(16, FW_BOLD, L"Segoe UI")};
  const wchar_t* words[] = {L"원격", L"desktop", L"선명도", L"report", L"2026-10-25", L"합계", L"₩1,234,500", L"session", L"문서", L"quality", L"지도", L"scroll"};
  int y = 8;
  for (int line = 0; y < height - 20; ++line) {
    SelectObject(dib.dc, fonts[(line + seed) % 3]); SetTextColor(dib.dc, RGB(20 + 30 * ((line + seed) % 3), 20 + 40 * (line % 2), 30 + 70 * (line % 3)));
    std::wstring text = std::to_wstring(line + 1) + L"  ";
    for (int k = 0; k < 24; ++k) text += std::wstring(words[(line * 7 + k * 3 + seed * 5) % 12]) + L" ";
    TextOutW(dib.dc, 16, y, text.c_str(), int(text.size())); y += 14 + 4 * ((line + seed) % 3);
  }
  for (auto f : fonts) DeleteObject(f);
  return dib.bytes();
}
std::vector<uint8_t> mapTexture(int w, int h) {
  std::vector<uint8_t> value(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    auto* p = &value[(size_t(y) * w + x) * 4];
    const bool road = (x % 97) < 4 || (y % 83) < 3 || std::abs((x + y) % 211 - 105) < 2;
    const int park = (x / 150 + y / 120) % 5 == 0;
    p[2] = uint8_t(road ? 250 : park ? 120 : 236 - (x * 13 + y * 7) % 23); p[1] = uint8_t(road ? 210 : park ? 200 : 232 - (x * 5 + y * 11) % 19); p[0] = uint8_t(road ? 120 : park ? 120 : 220); p[3] = 255;
  }
  return value;
}
void blit(std::vector<uint8_t>& frame, const std::vector<uint8_t>& source, int sourceWidth, int sx, int sy, int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; ++y) std::memcpy(&frame[(size_t(y) * width + x0) * 4], &source[(size_t(y - y0 + sy) * sourceWidth + sx) * 4], size_t(x1 - x0) * 4);
}
void drawCursor(std::vector<uint8_t>& frame, int x, int y) {
  Dib dib; std::memcpy(dib.pixels, frame.data(), frame.size());
  DrawIconEx(dib.dc, x, y, LoadCursorW(nullptr, IDC_ARROW), 32, 32, 0, nullptr, DI_NORMAL);
  frame = dib.bytes();
}

double psnr(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  double sum = 0; for (size_t i = 0; i < bgra.size(); i += 4) for (int c = 0; c < 3; ++c) { const double e = double(bgra[i + 2 - c]) - rgba[i + c]; sum += e * e; }
  const double mse = sum / (double(width) * height * 3); return mse == 0 ? INFINITY : 10 * std::log10(255.0 * 255.0 / mse);
}
bool exact(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  for (size_t i = 0; i < bgra.size(); i += 4) if (rgba[i] != bgra[i + 2] || rgba[i + 1] != bgra[i + 1] || rgba[i + 2] != bgra[i]) return false;
  return true;
}

struct Run {
  // The MFT keeps both pipelines' refinement decisions deterministic, so the
  // exact "tiles never change video submissions" check stays meaningful; x264
  // output timing can start one more refinement round run to run (its own
  // behaviour is covered by MultitapHostX264EncoderTest).
  explicit Run(bool tiles) : encoder(width, height, fps, bitrate, {}, host::EncoderBackend::mft), policy(size_t(width) * height * 3 / 2, host::FrameRefinement::forCbr(bitrate, fps)) {
    host::check(decoder.Initialize(width, height), "Decoder");
    require(compositor.Configure(width, height), "Compositor");
    if (tiles) source = std::make_unique<host::LosslessTileSource>(width, height, host::LosslessSettings::forBitrate(bitrate));
  }
  host::Encoder encoder; host::FrameEncodePolicy policy; multitap::RemoteDecoder decoder; multitap::lossless::Compositor compositor;
  std::unique_ptr<host::LosslessTileSource> source; host::ParallelWork helper;
  std::vector<uint8_t> published; std::vector<uint64_t> submitted; std::map<int, size_t> videoBytes, tileBytes;
  double composeMs = 0, composeMaxMs = 0, republishMs = 0; uint64_t decodedFrames = 0, republishes = 0, pendingAfterEncode = 0;
  // Host time: encode() wall time, tile phases, and how much the tile phases
  // lengthened the path of a submitted video frame (parallel: max(0, tile - encode)).
  double encodeMs = 0, tileMs = 0, extensionMs = 0, extensionMaxMs = 0, serialTileMs = 0, serialTileMaxMs = 0;
  std::map<int, double> tileMsAt;
  // `changed`: like WGC, a tick without a new surface frame has nothing to compare.
  void tick(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& nv12, int n, bool changed) {
    const uint64_t now = tickUs(n);
    host::BufferComposedFrame frame(bgra.data(), width, height, changed ? std::nullopt : std::optional<std::vector<host::capture::PixelRect>>(std::vector<host::capture::PixelRect>{}));
    std::vector<host::Frame> outputs;
    const auto encode = [&] { encoder.encode(nv12.data(), now, [&](host::Frame output) { outputs.push_back(std::move(output)); }); };
    host::FrameEncodeDecision decision;
    if (source) {
      const auto tick = host::losslessTick(*source, helper, frame, policy, nv12, now, encode);
      decision = tick.decision; encodeMs += tick.encodeMs; tileMs += tick.tileMs; tileMsAt[n] += tick.tileMs;
      if (decision.submit) { const double extra = std::max(0.0, tick.tileMs - tick.encodeMs); extensionMs += extra; extensionMaxMs = std::max(extensionMaxMs, extra); }
      else { serialTileMs += tick.tileMs; serialTileMaxMs = std::max(serialTileMaxMs, tick.tileMs); }
      if (const auto& payload = tick.payload) {
        tileBytes[n] += payload->size();
        require(compositor.Accept(payload->data(), payload->size()) == multitap::lossless::Compositor::Outcome::accepted, "Client rejected a host tile packet");
        if (compositor.republishPending() && !published.empty()) { const auto began = Clock::now(); compositor.ComposeAdded(published.data(), size_t(width) * 4); republishMs += since(began); ++republishes; }
      }
    } else {
      decision = policy.prepare(nv12, now);
      if (decision.submit) { const auto began = Clock::now(); encode(); encodeMs += since(began); }
    }
    if (decision.submit) {
      submitted.push_back(now);
      for (auto& output : outputs) {
        require(policy.output(output.ptsUs, output.key, now, output.bytes.size()), "Actual encoder output rejected");
        videoBytes[n] += output.bytes.size();
        host::check(decoder.Decode(output.bytes.data(), output.bytes.size(), output.ptsUs, [&](multitap::DecodedFrame&& decoded) {
          require(decoded.width == UINT(width) && decoded.height == UINT(height), "Decoded size");
          const std::vector<uint8_t> video = decoded.rgba;  // pristine copy for the stale check
          const auto began = Clock::now();
          if (compositor.active()) compositor.ComposeDecoded(decoded.rgba.data(), size_t(width) * 4, decoded.pts_us);
          const double ms = since(began); composeMs += ms; composeMaxMs = std::max(composeMaxMs, ms); ++decodedFrames;
          // Never a stale tile: every pixel is the exact source of this frame or its decoded video.
          for (size_t i = 0; i < bgra.size(); i += 4) {
            const uint8_t* d = &decoded.rgba[i];
            const bool same = d[0] == bgra[i + 2] && d[1] == bgra[i + 1] && d[2] == bgra[i];
            if (!same && std::memcmp(d, &video[i], 3)) throw std::runtime_error("A stale lossless tile overlaid newer video");
          }
          published = std::move(decoded.rgba);
        }), "Actual client decode");
      }
      pendingAfterEncode += policy.pendingInputs();
    }
  }
  size_t sum(const std::map<int, size_t>& bytes, int from, int to) const { size_t value = 0; for (const auto& [n, b] : bytes) if (n >= from && n < to) value += b; return value; }
  size_t submitsIn(int from, int to) const { size_t value = 0; for (const auto pts : submitted) value += pts >= tickUs(from) && pts < tickUs(to); return value; }
  std::vector<uint64_t> submissions(int from, int to) const { std::vector<uint64_t> value; for (const auto pts : submitted) if (pts >= tickUs(from) && pts < tickUs(to)) value.push_back(pts); return value; }
};
}

int main() {
  try {
    host::Runtime runtime;
    const auto desktop = desktopPage(), document = documentPage(2), map = mapTexture(2000, 1000);
    Run plain(false), tiles(true);
    // Scenes (60 Hz ticks): 0 desktop (static) | 180 page switch (static) |
    // 360 scroll | 540 map pan | 720 typing | 900 cursor moving | 1020 all static to 1260.
    struct Phase { const char* name; int from, to; };
    const Phase phases[] = {{"desktopSettle", 0, 180}, {"pageSwitchSettle", 180, 360}, {"scroll", 360, 540}, {"mapPan", 540, 720}, {"typing", 720, 900}, {"cursor", 900, 1020}, {"finalSettle", 1020, 1260}};
    std::map<std::string, int> lossless; std::map<std::string, double> plainPsnr, tilesPsnr;
    std::vector<uint8_t> frame, previous, nv12(size_t(width) * height * 3 / 2);
    int typed = 0;
    for (int n = 0; n < 1260; ++n) {
      if (n < 180) frame = desktop;
      else if (n < 360) frame = document;
      else if (n < 540) { frame = document; blit(frame, document, width, 0, ((n - 360) * 4) % 300, 0, 120, width, 900); }  // a scrolling pane
      else if (n < 720) { frame = desktop; blit(frame, map, 2000, (n - 540) * 3, (n - 540) * 2, 560, 120, 1840, 600); }  // panning map inside the window
      else if (n < 900) {
        frame = desktop; if ((n - 720) % 9 == 0) ++typed;
        Dib dib; std::memcpy(dib.pixels, frame.data(), frame.size()); HFONT f = font(14, FW_NORMAL, L"Malgun Gothic"); SelectObject(dib.dc, f); SetTextColor(dib.dc, RGB(0, 0, 0));
        RECT line{580, 1000, 1840, 1024}; FillRect(dib.dc, &line, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const std::wstring text = std::wstring(L"선명하게 입력되는 글자 typing test 네이버 웨일 ").substr(0, size_t(typed % 34));
        TextOutW(dib.dc, 584, 1004, text.c_str(), int(text.size())); DeleteObject(f); frame = dib.bytes();
      } else {
        frame = desktop; const int k = std::min(n, 1020) - 900;
        drawCursor(frame, 700 + k * 7, 300 + int(80 * std::sin(k / 9.0)));
      }
      host::bgraToNv12(frame.data(), nv12.data(), width, height);
      const bool changed = frame != previous; previous = frame;
      plain.tick(frame, nv12, n, changed); tiles.tick(frame, nv12, n, changed);
      for (const auto& phase : phases) {
        if (n >= phase.from && n < phase.to && !lossless.contains(phase.name) && !tiles.published.empty() && exact(frame, tiles.published)) lossless[phase.name] = n - phase.from;
        if (n == phase.to - 1) { plainPsnr[phase.name] = psnr(frame, plain.published); tilesPsnr[phase.name] = psnr(frame, tiles.published); }
      }
    }
    const auto& stats = tiles.source->stats();
    const auto ms = [](int ticks) { return ticks < 0 ? -1.0 : ticks * 1000.0 / fps; };
    std::ostringstream out; out << std::fixed << std::setprecision(2);
    out << "{\"kind\":\"lossless-tiles-encoder-loop\",\"resolution\":\"1920x1080\",\"fps\":60,\"bitrate\":12000000,\"phases\":[";
    for (size_t p = 0; p < std::size(phases); ++p) {
      const auto& phase = phases[p]; const auto found = lossless.find(phase.name);
      if (p) out << ',';
      out << "{\"name\":\"" << phase.name << "\",\"timeToLosslessMs\":" << (found == lossless.end() ? -1.0 : ms(found->second))
          << ",\"psnrEnd\":{\"original\":" << plainPsnr[phase.name] << ",\"tiles\":" << (std::isinf(tilesPsnr[phase.name]) ? std::string("\"inf\"") : std::to_string(tilesPsnr[phase.name])) << '}'
          << ",\"submits\":{\"original\":" << plain.submitsIn(phase.from, phase.to) << ",\"tiles\":" << tiles.submitsIn(phase.from, phase.to) << '}'
          << ",\"videoBytes\":{\"original\":" << plain.sum(plain.videoBytes, phase.from, phase.to) << ",\"tiles\":" << tiles.sum(tiles.videoBytes, phase.from, phase.to) << '}'
          << ",\"tileBytes\":" << tiles.sum(tiles.tileBytes, phase.from, phase.to) << ",\"hostTileMs\":" << [&] { double v = 0; for (const auto& [k, value] : tiles.tileMsAt) if (k >= phase.from && k < phase.to) v += value; return v; }() << '}';
    }
    out << "],\"host\":{\"tilePackets\":" << stats.packets << ",\"addedTiles\":" << stats.addedTiles << ",\"droppedTiles\":" << stats.droppedTiles << ",\"dropTicks\":" << stats.dropTicks
        << ",\"compareBeforeTotalMs\":" << stats.beforeEncodeMs << ",\"compareBeforeMaxMs\":" << stats.beforeEncodeMaxMs
        << ",\"payloadTotalMs\":" << stats.payloadMs << ",\"compareAfterTotalMs\":" << stats.afterEncodeMs
        << ",\"tilePhasesTotalMs\":" << tiles.tileMs << ",\"encodeTotalMs\":{\"original\":" << plain.encodeMs << ",\"tiles\":" << tiles.encodeMs << '}'
        << ",\"videoPathExtensionMs\":{\"total\":" << tiles.extensionMs << ",\"max\":" << tiles.extensionMaxMs << '}'
        << ",\"tilePhasesWithoutVideoMs\":{\"total\":" << tiles.serialTileMs << ",\"max\":" << tiles.serialTileMaxMs << '}'
        << ",\"pendingInputsAfterEncode\":" << tiles.pendingAfterEncode << '}'
        << ",\"client\":{\"decodedFrames\":" << tiles.decodedFrames << ",\"composeTotalMs\":" << tiles.composeMs << ",\"composeMaxMs\":" << tiles.composeMaxMs
        << ",\"republishes\":" << tiles.republishes << ",\"republishTotalMs\":" << tiles.republishMs << '}'
        << ",\"syntheticPixels\":true,\"actualMicrosoftCpuEncoder\":true,\"actualClientDecoder\":true,\"desktopCaptured\":false,\"relay\":false,\"realSessionAcceptance\":false}\n";
    std::cout << out.str();
    // Acceptance on this synthetic loop.
    for (const char* settle : {"desktopSettle", "pageSwitchSettle", "finalSettle"}) {
      require(lossless.contains(settle), "A static scene never became pixel-identical");
      require(std::isinf(tilesPsnr[settle]), "A static scene was not pixel-identical at its end");
    }
    require(lossless["pageSwitchSettle"] * 1000 / fps <= 2000, "Page switch took longer than 2 s to become lossless");
    for (const char* motion : {"scroll", "mapPan", "typing", "cursor"}) {
      const auto& phase = *std::find_if(std::begin(phases), std::end(phases), [&](const Phase& p) { return std::string(p.name) == motion; });
      require(plain.submissions(phase.from, phase.to) == tiles.submissions(phase.from, phase.to), "Motion video submissions differ from the original pipeline");
    }
    require(tiles.pendingAfterEncode == 0 && plain.pendingAfterEncode == 0, "The software encoder held an input (tile ordering assumption)");
    require(plain.submissions(0, 1260) == tiles.submissions(0, 1260) || stats.dropTicks > 0, "Unexplained submission difference");
    return 0;
  } catch (const std::exception& error) { std::cerr << "lossless encoder loop: " << error.what() << '\n'; return 1; }
}
