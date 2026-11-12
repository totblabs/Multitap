// Scaled-video lossless tiles at a field geometry: a
// 2948x1112 remote viewport encoded at 1920x724 under the default 1920 cap.
// Production CPU H.264 MFT + refinement, host LosslessTileSource on the UNSCALED
// source, the client's RemoteDecoder and scaled lossless::Compositor (video
// upscaled under absent tiles), against the alternatives: no tiles, and the
// cap raised so video itself is encoded at 2948x1112. GDI HALFTONE downscale as
// in Capture's scaled path. Synthetic pixels; no capture, relay or real session.
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
constexpr int sourceWidth = 2948, sourceHeight = 1112, scaledWidth = 1920, scaledHeight = 724, fps = 60, bitrate = 12000000, ticks = 780;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
uint64_t tickUs(int n) { return uint64_t(n + 1) * 1000000 / fps; }
using Clock = std::chrono::steady_clock;
double since(Clock::time_point began) { return std::chrono::duration<double, std::milli>(Clock::now() - began).count(); }

struct Dib {
  int width, height; HDC dc = CreateCompatibleDC(nullptr); HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr; uint8_t* pixels = nullptr;
  Dib(int w, int h) : width(w), height(h) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(dc && bitmap && pixels, "Synthetic canvas"); previous = SelectObject(dc, bitmap); SetBkMode(dc, TRANSPARENT);
  }
  ~Dib() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
  std::vector<uint8_t> bytes() { GdiFlush(); std::vector<uint8_t> value(pixels, pixels + size_t(width) * height * 4); for (size_t i = 3; i < value.size(); i += 4) value[i] = 255; return value; }
};
HFONT font(int size, int weight, const wchar_t* face) { return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face); }
std::vector<uint8_t> desktopPage() {
  Dib dib(sourceWidth, sourceHeight);
  for (int y = 0; y < sourceHeight; ++y) { HBRUSH brush = CreateSolidBrush(RGB(0, 70 + y * 60 / sourceHeight, 140 + y * 80 / sourceHeight)); RECT row{0, y, sourceWidth, y + 1}; FillRect(dib.dc, &row, brush); DeleteObject(brush); }
  HFONT label = font(12, FW_NORMAL, L"Segoe UI"), korean = font(12, FW_NORMAL, L"Malgun Gothic"), body = font(14, FW_NORMAL, L"Segoe UI");
  const wchar_t* names[] = {L"네이버 웨일", L"휴지통", L"내 PC", L"문서", L"Microsoft Edge", L"제어판", L"보고서.xlsx", L"지도"};
  LPCWSTR icons[] = {IDI_APPLICATION, IDI_INFORMATION, IDI_WARNING, IDI_ERROR, IDI_SHIELD, IDI_QUESTION, IDI_WINLOGO, IDI_ASTERISK};
  for (int n = 0; n < 27; ++n) {
    const int x = 24 + (n / 9) * 96, y = 16 + (n % 9) * 118;
    DrawIconEx(dib.dc, x + 20, y, LoadIconW(nullptr, icons[n % 8]), 48, 48, 0, nullptr, DI_NORMAL);
    SelectObject(dib.dc, n % 2 ? label : korean); SetTextColor(dib.dc, RGB(255, 255, 255));
    RECT text{x - 8, y + 54, x + 96, y + 90}; DrawTextW(dib.dc, names[n % 8], -1, &text, DT_CENTER | DT_WORDBREAK);
  }
  RECT window{640, 90, 2860, 1060}; FillRect(dib.dc, &window, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  for (int line = 0; line < 50; ++line) {
    SelectObject(dib.dc, line % 4 ? body : korean); SetTextColor(dib.dc, RGB(20 + (line * 47) % 200, 30, 60 + (line * 23) % 160));
    std::wstring text = std::to_wstring(line + 1) + L"  네이버 웨일 브라우저 · ToTBSeamless 원격 데스크톱 선명도 비교 · report 2026-10-25 ₩1,234,500 · wide viewport 2948x1112";
    TextOutW(dib.dc, 660, 100 + line * 19, text.c_str(), int(text.size()));
  }
  DeleteObject(label); DeleteObject(korean); DeleteObject(body);
  return dib.bytes();
}
std::vector<uint8_t> documentPage(int seed) {
  Dib dib(sourceWidth, sourceHeight + 400); RECT all{0, 0, sourceWidth, sourceHeight + 400}; FillRect(dib.dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  HFONT fonts[] = {font(12, FW_NORMAL, L"Segoe UI"), font(14, FW_NORMAL, L"Malgun Gothic"), font(16, FW_BOLD, L"Segoe UI")};
  const wchar_t* words[] = {L"원격", L"desktop", L"선명도", L"report", L"2026-10-25", L"합계", L"₩1,234,500", L"session", L"문서", L"quality", L"지도", L"scroll"};
  int y = 8;
  for (int line = 0; y < sourceHeight + 380; ++line) {
    SelectObject(dib.dc, fonts[(line + seed) % 3]); SetTextColor(dib.dc, RGB(20 + 30 * ((line + seed) % 3), 20 + 40 * (line % 2), 30 + 70 * (line % 3)));
    std::wstring text = std::to_wstring(line + 1) + L"  "; for (int k = 0; k < 36; ++k) text += std::wstring(words[(line * 7 + k * 3 + seed * 5) % 12]) + L" ";
    TextOutW(dib.dc, 16, y, text.c_str(), int(text.size())); y += 14 + 4 * ((line + seed) % 3);
  }
  for (auto f : fonts) DeleteObject(f);
  return dib.bytes();
}
// Capture's scaled path: StretchDIBits with HALFTONE into the encoded frame.
std::vector<uint8_t> downscale(const std::vector<uint8_t>& source) {
  Dib dib(scaledWidth, scaledHeight); SetStretchBltMode(dib.dc, HALFTONE); SetBrushOrgEx(dib.dc, 0, 0, nullptr);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = sourceWidth; info.bmiHeader.biHeight = -sourceHeight; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  require(StretchDIBits(dib.dc, 0, 0, scaledWidth, scaledHeight, 0, 0, sourceWidth, sourceHeight, source.data(), &info, DIB_RGB_COLORS, SRCCOPY) != 0, "Downscale");
  return dib.bytes();
}
double psnr(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  double sum = 0; for (size_t i = 0; i < bgra.size(); i += 4) for (int c = 0; c < 3; ++c) { const double e = double(bgra[i + 2 - c]) - rgba[i + c]; sum += e * e; }
  const double mse = sum / (double(bgra.size() / 4) * 3); return mse == 0 ? INFINITY : 10 * std::log10(255.0 * 255.0 / mse);
}
bool exact(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  if (bgra.size() != rgba.size()) return false;
  for (size_t i = 0; i < bgra.size(); i += 4) if (rgba[i] != bgra[i + 2] || rgba[i + 1] != bgra[i + 1] || rgba[i + 2] != bgra[i]) return false;
  return true;
}

struct Run {
  // MFT: deterministic refinement decisions for the exact submission check
  // (see lossless_tiles_encoder_test.cpp).
  Run(int encodedWidth, int encodedHeight, bool tiles) : ew(encodedWidth), eh(encodedHeight), encoder(ew, eh, fps, bitrate, {}, host::EncoderBackend::mft), policy(size_t(ew) * eh * 3 / 2, host::FrameRefinement::forCbr(bitrate, fps)) {
    host::check(decoder.Initialize(uint32_t(ew), uint32_t(eh)), "Decoder");
    require(compositor.Configure(uint32_t(ew), uint32_t(eh), true), "Compositor");
    if (ew != sourceWidth) require(gpu.Configure(uint32_t(ew), uint32_t(eh), sourceWidth, sourceHeight), "Display upscaler");
    if (tiles) source = std::make_unique<host::LosslessTileSource>(sourceWidth, sourceHeight, host::LosslessSettings::forBitrate(bitrate), uint32_t(ew), uint32_t(eh));
  }
  int ew, eh; host::Encoder encoder; host::FrameEncodePolicy policy; multitap::RemoteDecoder decoder; multitap::lossless::Compositor compositor;
  std::unique_ptr<host::LosslessTileSource> source; host::ParallelWork helper; multitap::video::BilinearUpscaler gpu;
  multitap::DecodedFrame published; std::map<int, size_t> videoBytes, tileBytes; std::vector<uint64_t> submitted;
  std::map<int, double> composeAt; double encodeMs = 0, motionEncodeMs = 0, tileMs = 0, extensionMs = 0, composeMs = 0, composeMaxMs = 0, republishMs = 0; uint64_t motionFrames = 0, decoded = 0, republishes = 0, scaledFrames = 0;
  // What the screen shows at source size: a source-size frame as is, a video-size frame bilinear-scaled (Flutter's low filter).
  std::vector<uint8_t> shown() {
    if (published.width == uint32_t(sourceWidth)) return published.rgba;
    std::vector<uint8_t> value(size_t(sourceWidth) * sourceHeight * 4);
    gpu.Resample(published.rgba.data(), size_t(ew) * 4, value.data(), size_t(sourceWidth) * 4, 0, 0, sourceWidth, sourceHeight); return value;
  }
  void tick(const std::vector<uint8_t>& full, const std::vector<uint8_t>& encodedBgra, const std::vector<uint8_t>& nv12, int n, bool changed, bool motion) {
    const uint64_t now = tickUs(n);
    host::BufferComposedFrame frame(full.data(), sourceWidth, sourceHeight, changed ? std::nullopt : std::optional<std::vector<host::capture::PixelRect>>(std::vector<host::capture::PixelRect>{}));
    std::vector<host::Frame> outputs;
    const auto encode = [&] { encoder.encode(nv12.data(), now, [&](host::Frame output) { outputs.push_back(std::move(output)); }); };
    host::FrameEncodeDecision decision; double frameEncodeMs = 0;
    if (source) {
      const auto tick = host::losslessTick(*source, helper, frame, policy, nv12, now, encode);
      decision = tick.decision; frameEncodeMs = tick.encodeMs; tileMs += tick.tileMs; if (decision.submit) extensionMs += std::max(0.0, tick.tileMs - tick.encodeMs);
      if (const auto& payload = tick.payload) {
        tileBytes[n] += payload->size();
        require(compositor.Accept(payload->data(), payload->size()) == multitap::lossless::Compositor::Outcome::accepted, "Client rejected a host tile packet");
        if (compositor.republishPending() && published.width) {
          const auto began = Clock::now();
          if (published.width == compositor.width()) compositor.ComposeAdded(published.rgba.data(), size_t(published.width) * 4);
          else { multitap::DecodedFrame up; up.width = compositor.width(); up.height = compositor.height(); up.pts_us = published.pts_us; up.rgba.resize(size_t(up.width) * up.height * 4);
                 compositor.ComposeUpscaled(published.rgba.data(), size_t(published.width) * 4, up.rgba.data(), size_t(up.width) * 4); published = std::move(up); }
          republishMs += since(began); ++republishes;
        }
      }
    } else {
      decision = policy.prepare(nv12, now);
      if (decision.submit) { const auto began = Clock::now(); encode(); frameEncodeMs = since(began); }
    }
    encodeMs += frameEncodeMs; if (motion && decision.submit) { motionEncodeMs += frameEncodeMs; ++motionFrames; }
    if (!decision.submit) return;
    submitted.push_back(now);
    for (auto& output : outputs) {
      require(policy.output(output.ptsUs, output.key, now, output.bytes.size()), "Actual encoder output rejected");
      videoBytes[n] += output.bytes.size();
      host::check(decoder.Decode(output.bytes.data(), output.bytes.size(), output.ptsUs, [&](multitap::DecodedFrame&& video) {
        ++decoded; const auto began = Clock::now();
        if (compositor.active()) {
          if (compositor.scaled()) {
            multitap::DecodedFrame out; out.width = compositor.width(); out.height = compositor.height(); out.pts_us = video.pts_us; out.rgba.resize(size_t(out.width) * out.height * 4);
            compositor.ComposeDecodedScaled(video.rgba.data(), size_t(video.width) * 4, out.rgba.data(), size_t(out.width) * 4, video.pts_us);
            ++scaledFrames; const double ms = since(began); composeMs += ms; composeAt[n] += ms; composeMaxMs = std::max(composeMaxMs, ms);
            // Never a stale tile: every pixel is the exact source of this frame or the upscaled video.
            std::vector<uint8_t> upscaled(out.rgba.size()); gpu.Resample(video.rgba.data(), size_t(video.width) * 4, upscaled.data(), size_t(out.width) * 4, 0, 0, out.width, out.height);
            for (size_t i = 0; i < full.size(); i += 4) {
              const uint8_t* d = &out.rgba[i]; const bool same = d[0] == full[i + 2] && d[1] == full[i + 1] && d[2] == full[i];
              if (!same && std::memcmp(d, &upscaled[i], 3)) throw std::runtime_error("A stale lossless tile overlaid newer scaled video");
            }
            published = std::move(out); return;
          }
          const std::vector<uint8_t> pristine = video.rgba;
          compositor.ComposeDecoded(video.rgba.data(), size_t(video.width) * 4, video.pts_us);
          const double ms = since(began); composeMs += ms; composeAt[n] += ms; composeMaxMs = std::max(composeMaxMs, ms);
          for (size_t i = 0; i < full.size(); i += 4) {
            const uint8_t* d = &video.rgba[i]; const bool same = d[0] == full[i + 2] && d[1] == full[i + 1] && d[2] == full[i];
            if (!same && std::memcmp(d, &pristine[i], 3)) throw std::runtime_error("A stale lossless tile overlaid newer video");
          }
        }
        published = std::move(video);
      }), "Actual client decode");
    }
    (void)encodedBgra;
  }
  size_t sum(const std::map<int, size_t>& bytes, int from, int to) const { size_t value = 0; for (const auto& [n, b] : bytes) if (n >= from && n < to) value += b; return value; }
  std::vector<uint64_t> submissions(int from, int to) const { std::vector<uint64_t> value; for (const auto pts : submitted) if (pts >= tickUs(from) && pts < tickUs(to)) value.push_back(pts); return value; }
};
}

int main() {
  try {
    host::Runtime runtime;
    const auto desktop = desktopPage(), document = documentPage(3);
    struct Phase { const char* name; int from, to; bool motion; };
    const Phase phases[] = {{"desktopSettle", 0, 180, false}, {"pageSwitchSettle", 180, 360, false}, {"scroll", 360, 480, true}, {"paneScrollOverLosslessDesktop", 480, 660, true}, {"finalSettle", 660, 780, false}};
    const char* names[] = {"scaledOriginal", "scaledTiles", "capRaisedOriginal", "capRaisedTiles"};
    std::vector<std::unique_ptr<Run>> runs;
    runs.push_back(std::make_unique<Run>(scaledWidth, scaledHeight, false)); runs.push_back(std::make_unique<Run>(scaledWidth, scaledHeight, true));
    runs.push_back(std::make_unique<Run>(sourceWidth, sourceHeight, false)); runs.push_back(std::make_unique<Run>(sourceWidth, sourceHeight, true));
    std::map<std::pair<int, std::string>, int> lossless; std::map<std::pair<int, std::string>, double> quality; std::map<int, bool> staticDuringMotion;
    std::vector<uint8_t> frame, previous, scaledNv12(size_t(scaledWidth) * scaledHeight * 3 / 2), fullNv12(size_t(sourceWidth) * sourceHeight * 3 / 2);
    for (int n = 0; n < ticks; ++n) {
      if (n < 180) frame = desktop;
      else if (n < 360) frame = std::vector<uint8_t>(document.begin(), document.begin() + std::ptrdiff_t(size_t(sourceWidth) * sourceHeight * 4));
      else if (n >= 480) {
        // The desktop (icons, lossless) with its text window scrolling, then still.
        frame = desktop; const int offset = n < 660 ? ((std::min(n, 659) - 479) * 3) % 400 : 0;
        for (int y = 90; y < 1060; ++y) std::memcpy(&frame[(size_t(y) * sourceWidth + 640) * 4], &document[(size_t(y + offset) * sourceWidth + 16) * 4], size_t(2860 - 640) * 4);
      } else { const int offset = ((n - 359) * 3) % 400; frame.assign(document.begin() + std::ptrdiff_t(size_t(offset) * sourceWidth * 4), document.begin() + std::ptrdiff_t(size_t(offset + sourceHeight) * sourceWidth * 4)); }
      const bool changed = frame != previous; previous = frame;
      const auto scaled = downscale(frame);
      host::bgraToNv12(scaled.data(), scaledNv12.data(), scaledWidth, scaledHeight); host::bgraToNv12(frame.data(), fullNv12.data(), sourceWidth, sourceHeight);
      const bool motion = n >= 360 && n < 660;
      runs[0]->tick(frame, scaled, scaledNv12, n, changed, motion); runs[1]->tick(frame, scaled, scaledNv12, n, changed, motion);
      runs[2]->tick(frame, frame, fullNv12, n, changed, motion); runs[3]->tick(frame, frame, fullNv12, n, changed, motion);
      for (size_t r = 0; r < runs.size(); ++r) for (const auto& phase : phases) {
        const auto key = std::make_pair(int(r), std::string(phase.name));
        if ((r == 1 || r == 3) && n >= phase.from && n < phase.to && !lossless.contains(key) && runs[r]->published.width == uint32_t(sourceWidth) && exact(frame, runs[r]->published.rgba)) lossless[key] = n - phase.from;
        if (n == phase.to - 1) quality[key] = psnr(frame, runs[r]->shown());
      }
      if (n == 659) for (const int r : {1, 3}) {
        // The icon column left of the scrolling window (tile columns 0-9) while it still scrolls.
        const auto shown = runs[r]->shown(); bool same = true;
        for (int y = 0; y < sourceHeight && same; ++y) for (int x = 0; x < 640 && same; ++x) { const size_t i = (size_t(y) * sourceWidth + x) * 4; same = shown[i] == frame[i + 2] && shown[i + 1] == frame[i + 1] && shown[i + 2] == frame[i]; }
        staticDuringMotion[r] = same;
      }
    }
    std::ostringstream out; out << std::fixed << std::setprecision(2);
    out << "{\"kind\":\"lossless-tiles-scaled-encoder-loop\",\"source\":\"2948x1112\",\"defaultCapVideo\":\"1920x724\",\"fps\":60,\"bitrate\":12000000,\"runs\":[";
    for (size_t r = 0; r < runs.size(); ++r) {
      auto& run = *runs[r]; if (r) out << ',';
      out << "{\"name\":\"" << names[r] << "\",\"video\":\"" << run.ew << 'x' << run.eh << "\",\"phases\":[";
      for (size_t p = 0; p < std::size(phases); ++p) {
        const auto& phase = phases[p]; const auto key = std::make_pair(int(r), std::string(phase.name)); if (p) out << ',';
        const auto found = lossless.find(key); const double q = quality[key];
        out << "{\"name\":\"" << phase.name << "\",\"timeToLosslessMs\":" << (found == lossless.end() ? -1.0 : found->second * 1000.0 / fps)
            << ",\"psnrEnd\":" << (std::isinf(q) ? std::string("\"inf\"") : std::to_string(q)) << ",\"submits\":" << run.submissions(phase.from, phase.to).size()
            << ",\"videoBytes\":" << run.sum(run.videoBytes, phase.from, phase.to) << ",\"tileBytes\":" << run.sum(run.tileBytes, phase.from, phase.to)
            << ",\"clientComposeMs\":" << [&] { double v = 0; for (const auto& [k, ms] : run.composeAt) if (k >= phase.from && k < phase.to) v += ms; return v; }() << '}';
      }
      out << "],\"hostEncodeTotalMs\":" << run.encodeMs << ",\"hostMotionEncodeMsPerFrame\":" << (run.motionFrames ? run.motionEncodeMs / double(run.motionFrames) : 0.0)
          << ",\"hostTileTotalMs\":" << run.tileMs << ",\"videoPathExtensionMs\":" << run.extensionMs << ",\"clientComposeTotalMs\":" << run.composeMs << ",\"clientComposeMaxMs\":" << run.composeMaxMs
          << ",\"clientScaledFrames\":" << run.scaledFrames << ",\"clientDecodedFrames\":" << run.decoded << ",\"republishes\":" << run.republishes << ",\"republishTotalMs\":" << run.republishMs << '}';
    }
    out << "],\"staticColumnExactWhilePaneScrolls\":{\"scaledTiles\":" << (staticDuringMotion[1] ? "true" : "false") << ",\"capRaisedTiles\":" << (staticDuringMotion[3] ? "true" : "false") << "},\"syntheticPixels\":true,\"actualMicrosoftCpuEncoder\":true,\"actualClientDecoder\":true,\"desktopCaptured\":false,\"relay\":false,\"realSessionAcceptance\":false}\n";
    std::cout << out.str();
    for (const int r : {1, 3}) for (const char* settle : {"desktopSettle", "pageSwitchSettle", "finalSettle"})
      require(lossless.contains({r, settle}) && std::isinf(quality[{r, settle}]), "A static scaled scene did not become pixel-identical at source resolution");
    require(staticDuringMotion[1] && staticDuringMotion[3], "Static content did not become lossless while another region moved");
    require(runs[0]->submissions(0, ticks) == runs[1]->submissions(0, ticks) && runs[2]->submissions(0, ticks) == runs[3]->submissions(0, ticks), "Tiles changed the video submissions");
    return 0;
  } catch (const std::exception& error) { std::cerr << "lossless scaled loop: " << error.what() << '\n'; return 1; }
}
