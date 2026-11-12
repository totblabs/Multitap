// x264 production backend (docs/x264-encoder.md) against the client's actual
// RemoteDecoder (Microsoft H.264 decoder): the bitstream contract shared with
// the MFT path, IDR placement, a live bitrate change, a resize epoch, the VBV
// bound against the 4 MiB media packet, High/CABAC versus constrained baseline
// on the same decoder, and the automatic x264 -> MFT fallback. Synthetic pixels
// on a synthetic 60 Hz clock; no desktop is read. Not a real-session, relay or
// latency result.
#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "x264_encoder_testing.hpp"
#include "x264_constant_blocks.hpp"
#include "remote_decoder.h"
#include "reference_history.hpp"
#include "../../../native/windows_video/color.hpp"
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <string>

namespace {
constexpr int fps = 60;
std::atomic<uint64_t> configurationProviderCalls{0};
host::EncoderBackend cpuFixtureConfiguration() { ++configurationProviderCalls; return host::EncoderBackend::x264; }
struct CpuConfigurationScope {
  CpuConfigurationScope() {
    if (host::x264testing::configurationProvider.load()) throw std::runtime_error("CPU fixture configuration was already installed");
    host::x264testing::configurationProvider.store(cpuFixtureConfiguration);
  }
  ~CpuConfigurationScope() { host::x264testing::configurationProvider.store(nullptr); }
};
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); }
uint64_t tickUs(int n, int rate = fps) { return uint64_t(n) * 1000000 / uint64_t(rate); }

// ------------------------------------------------------------ synthetic content
// ClearType text page like a default Windows desktop (as encoder_bitrate_test).
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
  const wchar_t* words[] = {L"desktop", L"report", L"2026-10-26", L"session", L"quality", L"scroll", L"문서", L"합계", L"지도", L"1,234,500", L"relay", L"x264"};
  // Coloured blocks so a wrong matrix/range would show in the colour PSNR.
  for (int k = 0; k < 6; ++k) { RECT block{width - 220, 20 + 60 * k, width - 40, 70 + 60 * k}; HBRUSH brush = CreateSolidBrush(RGB(40 * k, 255 - 35 * k, (90 * k) % 256)); FillRect(dc, &block, brush); DeleteObject(brush); }
  int y = 8;
  for (int line = 0; y < height - 20; ++line) {
    SelectObject(dc, fonts[(line + seed) % 3]); SetTextColor(dc, RGB(20 + 30 * ((line + seed) % 3), 20 + 50 * (line % 2), 30 + 40 * (seed % 2)));
    std::wstring text = std::to_wstring(line + 1) + L"  ";
    for (int k = 0; k < (width - 260) / 60; ++k) text += std::wstring(words[(line * 7 + k * 3 + seed * 5) % 12]) + L" ";
    TextOutW(dc, 16, y, text.c_str(), int(text.size())); y += 14 + 4 * ((line + seed) % 3);
  }
  GdiFlush();
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2); host::bgraToNv12(pixels, nv12.data(), width, height);
  SelectObject(dc, previous); for (auto font : fonts) DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
  return nv12;
}
// A tall page scrolled 8 px per frame (luma rows and chroma rows move together).
struct Scroll {
  int width, height; std::vector<uint8_t> tall, frame;
  Scroll(int w, int h, int seed) : width(w), height(h), tall(page(w, h * 3, seed)), frame(size_t(w) * h * 3 / 2) {}
  const std::vector<uint8_t>& at(int n) {
    const int offset = (n * 8) % (2 * height) & ~1;
    for (int y = 0; y < height; ++y) std::memcpy(frame.data() + size_t(y) * width, tall.data() + size_t(y + offset) * width, size_t(width));
    for (int y = 0; y < height / 2; ++y) std::memcpy(frame.data() + size_t(width) * height + size_t(y) * width, tall.data() + size_t(width) * height * 3 + size_t(y + offset / 2) * width, size_t(width));
    return frame;
  }
};
// Detailed moving content whose cost scales with QP (encoder_bitrate_test).
std::vector<uint8_t> grain(int width, int height, int n) {
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2); uint32_t seed = 2463534242u ^ uint32_t(n * 2654435761u);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const int base = y < height / 2 ? (((x + n * 5) / 6 + y / 6) % 2 ? 180 : 60) : 64 + (x + y + n * 3) % 128;
    const bool noisy = x < 1920 && y >= height / 2 && y < height / 2 + 540;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    nv12[size_t(y) * width + x] = uint8_t(std::clamp(base + (noisy ? int(seed % 9) - 4 : 0), 16, 235));
  }
  uint8_t* chroma = nv12.data() + size_t(width) * height;
  for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width; x += 2) { chroma[size_t(y) * width + x] = uint8_t(128 + ((x + n) % 64) - 32); chroma[size_t(y) * width + x + 1] = uint8_t(128 + ((y + n) % 48) - 24); }
  return nv12;
}
// Incompressible full-range noise: the worst case for the VBV bound.
std::vector<uint8_t> noise(int width, int height, uint32_t seed) {
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
  for (auto& value : nv12) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; value = uint8_t(16 + seed % 220); }
  return nv12;
}
double psnr(int width, int height, const std::vector<uint8_t>& source, const std::vector<uint8_t>& decoded) {
  std::vector<uint8_t> reference(size_t(width) * height * 4);
  multitap::video::nv12ToRgba(source.data(), reference.data(), width, height, width, height, false, false);
  double sum = 0; for (size_t i = 0; i < reference.size(); ++i) if (i % 4 != 3) { const double e = double(reference[i]) - decoded[i]; sum += e * e; }
  const double mse = sum / (double(width) * height * 3); return mse < 1e-9 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}
// PSNR of the saturated colour blocks of page() only: the client converts with
// BT.601 limited range, so a stream the decoder interpreted with another
// matrix/range shows a large error here (neutral text would hide it).
double colourPsnr(int width, int height, const std::vector<uint8_t>& source, const std::vector<uint8_t>& decoded) {
  std::vector<uint8_t> reference(size_t(width) * height * 4);
  multitap::video::nv12ToRgba(source.data(), reference.data(), width, height, width, height, false, false);
  double sum = 0; size_t samples = 0;
  for (int k = 0; k < 6; ++k) for (int y = 24 + 60 * k; y < 66 + 60 * k; ++y) for (int x = width - 216; x < width - 44; ++x)
    for (int c = 0; c < 3; ++c) { const size_t at = (size_t(y) * width + x) * 4 + c; const double e = double(reference[at]) - decoded[at]; sum += e * e; ++samples; }
  const double mse = sum / double(samples); return mse < 1e-9 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

// ------------------------------------------------------------ bitstream parsing
struct Nal { int type; std::vector<uint8_t> rbsp; };
std::vector<Nal> nals(const std::vector<uint8_t>& bytes) {
  std::vector<std::pair<size_t, size_t>> starts;  // payload start, prefix start
  for (size_t i = 0; i + 3 <= bytes.size(); ++i)
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) { starts.push_back({i + 3, i > 0 && bytes[i - 1] == 0 ? i - 1 : i}); i += 2; }
  std::vector<Nal> result;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t begin = starts[k].first, end = k + 1 < starts.size() ? starts[k + 1].second : bytes.size();
    if (begin >= end) continue;
    Nal nal{bytes[begin] & 31, {}}; int zeros = 0;
    for (size_t i = begin + 1; i < end; ++i) { if (zeros >= 2 && bytes[i] == 3) { zeros = 0; continue; } nal.rbsp.push_back(bytes[i]); zeros = bytes[i] == 0 ? zeros + 1 : 0; }
    result.push_back(std::move(nal));
  }
  return result;
}
struct Bits {
  const std::vector<uint8_t>& data; size_t at = 0;
  uint32_t u(int n) { uint32_t v = 0; for (int i = 0; i < n; ++i) { require(at < data.size() * 8, "Truncated parameter set"); v = (v << 1) | ((data[at / 8] >> (7 - at % 8)) & 1); ++at; } return v; }
  uint32_t ue() { int zeros = 0; while (!u(1)) require(++zeros < 32, "Invalid Exp-Golomb code"); return (1u << zeros) - 1 + u(zeros); }
  int32_t se() { const uint32_t k = ue(); return k & 1 ? int32_t((k + 1) / 2) : -int32_t(k / 2); }
};
struct Sps { int profile = -1, constraints = 0, level = 0, refs = 0, width = 0, height = 0; bool vui = false, signal = false, fullRange = false, colour = false, restriction = false; int format = 5, primaries = 2, transfer = 2, matrix = 2, reorder = -1; };
void hrd(Bits& b) { const auto count = b.ue(); b.u(4); b.u(4); for (uint32_t i = 0; i <= count; ++i) { b.ue(); b.ue(); b.u(1); } b.u(20); }
Sps parseSps(const std::vector<uint8_t>& rbsp) {
  Bits b{rbsp}; Sps s; s.profile = int(b.u(8)); s.constraints = int(b.u(8)); s.level = int(b.u(8)); b.ue();
  if (s.profile == 100 || s.profile == 110 || s.profile == 122 || s.profile == 244 || s.profile == 44 || s.profile == 83 || s.profile == 86 || s.profile == 118 || s.profile == 128) {
    const auto chroma = b.ue(); require(chroma == 1, "Not 4:2:0"); b.ue(); b.ue(); b.u(1); require(b.u(1) == 0, "Unexpected scaling matrix");
  }
  b.ue(); const auto poc = b.ue();
  if (poc == 0) b.ue(); else if (poc == 1) { b.u(1); b.se(); b.se(); const auto cycle = b.ue(); for (uint32_t i = 0; i < cycle; ++i) b.se(); }
  s.refs = int(b.ue()); b.u(1);
  const auto mbWidth = b.ue() + 1, mbHeight = b.ue() + 1; const bool frames = b.u(1); require(frames, "Interlaced SPS"); b.u(1);
  int crop[4]{}; if (b.u(1)) for (auto& c : crop) c = int(b.ue());
  s.width = int(mbWidth * 16) - 2 * (crop[0] + crop[1]); s.height = int(mbHeight * 16) - 2 * (crop[2] + crop[3]);
  s.vui = b.u(1);
  if (!s.vui) return s;
  if (b.u(1)) { if (b.u(8) == 255) { b.u(16); b.u(16); } }
  if (b.u(1)) b.u(1);
  s.signal = b.u(1);
  if (s.signal) { s.format = int(b.u(3)); s.fullRange = b.u(1); s.colour = b.u(1); if (s.colour) { s.primaries = int(b.u(8)); s.transfer = int(b.u(8)); s.matrix = int(b.u(8)); } }
  if (b.u(1)) { b.ue(); b.ue(); }
  if (b.u(1)) { b.u(32); b.u(32); b.u(1); }
  const bool nal = b.u(1); if (nal) hrd(b);
  const bool vcl = b.u(1); if (vcl) hrd(b);
  if (nal || vcl) b.u(1);
  b.u(1);
  s.restriction = b.u(1);
  if (s.restriction) { b.u(1); b.ue(); b.ue(); b.ue(); b.ue(); s.reorder = int(b.ue()); b.ue(); }
  return s;
}
int sliceType(const std::vector<uint8_t>& rbsp) { Bits b{rbsp}; b.ue(); return int(b.ue() % 5); }

Sps sps(const host::Frame& frame) { for (const auto& nal : nals(frame.bytes)) if (nal.type == 7) return parseSps(nal.rbsp); return {}; }
struct StreamShape { std::vector<Sps> sps; size_t idr = 0, frames = 0, bSlices = 0, maxBytes = 0, ppsBeforeEveryIdr = 0, idrWithHeaders = 0; std::vector<size_t> idrIndices; };
void inspect(const host::Frame& frame, size_t index, StreamShape& shape) {
  const auto list = nals(frame.bytes); bool idr = false, sps = false, pps = false;
  for (const auto& nal : list) {
    if (nal.type == 7) { shape.sps.push_back(parseSps(nal.rbsp)); sps = true; }
    if (nal.type == 8) pps = true;
    if (nal.type == 5) idr = true;
    if ((nal.type == 1 || nal.type == 5) && sliceType(nal.rbsp) == 1) ++shape.bSlices;
  }
  require(idr == frame.key, "Frame key flag differs from its NAL types");
  if (idr) { ++shape.idr; shape.idrIndices.push_back(index); if (sps && pps) ++shape.idrWithHeaders; }
  ++shape.frames; shape.maxBytes = std::max(shape.maxBytes, frame.bytes.size());
}

// One encoder decoded by one client decoder, pts must come back unchanged.
struct Loop {
  int width, height; host::Encoder encoder; multitap::RemoteDecoder decoder; StreamShape shape;
  std::map<uint64_t, std::vector<uint8_t>> sources; std::map<uint64_t, double> quality, colour; std::map<uint64_t, size_t> sizes; bool scoreColour = false;
  size_t decoded = 0, encoded = 0, lastBytes = 0; bool lastKey = false; std::vector<uint64_t> outputs;
  Loop(int w, int h, int bitrate, const host::X264Settings& settings) : width(w), height(h), encoder(w, h, fps, bitrate, settings) { init(); }
  Loop(int w, int h, int bitrate, host::EncoderBackend backend, int rate = fps) : width(w), height(h), encoder(w, h, rate, bitrate, {}, backend) { init(); }
  void init() { host::check(decoder.Initialize(UINT(width), UINT(height)), "Initialize actual client decoder"); }
  void output(host::Frame frame) {
    inspect(frame, encoded, shape); ++encoded; outputs.push_back(frame.ptsUs); sizes[frame.ptsUs] = frame.bytes.size(); lastBytes = frame.bytes.size(); lastKey = frame.key;
    require(frame.bytes.size() <= host::encoderMaximumFrameBytes, "Encoded frame exceeds the media packet bound");
    host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& shown) {
      require(shown.width == UINT(width) && shown.height == UINT(height), "Decoded geometry changed"); ++decoded;
      if (const auto source = sources.find(shown.pts_us); source != sources.end()) {
        quality[shown.pts_us] = psnr(width, height, source->second, shown.rgba);
        if (scoreColour) colour[shown.pts_us] = colourPsnr(width, height, source->second, shown.rgba);
        sources.erase(source);
      }
    }), "Actual client decode");
  }
  void encode(const std::vector<uint8_t>& frame, uint64_t pts, bool score) {
    if (score) sources[pts] = frame;
    const size_t before = encoded;
    encoder.encode(frame.data(), pts, [&](host::Frame f) { require(f.ptsUs == pts, "Output pts differs from its input"); output(std::move(f)); });
    require(encoded == before + 1, "Encoder delayed or dropped a frame");
  }
  void finish() {
    encoder.finish([&](host::Frame f) { output(std::move(f)); });
    host::check(decoder.Drain([&](multitap::DecodedFrame&& shown) { ++decoded; if (const auto source = sources.find(shown.pts_us); source != sources.end()) { quality[shown.pts_us] = psnr(width, height, source->second, shown.rgba); sources.erase(source); } }), "Drain client decoder");
  }
  double meanQuality(uint64_t from, uint64_t to) const { double sum = 0; int n = 0; for (const auto& [pts, q] : quality) if (pts >= from && pts < to) { sum += q; ++n; } return n ? sum / n : 0; }
  double mbps(uint64_t from, uint64_t to) const { double bytes = 0; for (const auto& [pts, size] : sizes) if (pts >= from && pts < to) bytes += double(size); return bytes * 8 / (double(to - from) / 1e6) / 1e6; }
};

void printSps(const char* name, const Sps& s) {
  std::cout << "{\"kind\":\"sps\",\"stream\":\"" << name << "\",\"profileIdc\":" << s.profile << ",\"constraintFlags\":" << s.constraints << ",\"levelIdc\":" << s.level << ",\"refs\":" << s.refs
            << ",\"width\":" << s.width << ",\"height\":" << s.height << ",\"videoSignal\":" << s.signal << ",\"fullRange\":" << s.fullRange << ",\"colourDescription\":" << s.colour
            << ",\"primaries\":" << s.primaries << ",\"transfer\":" << s.transfer << ",\"matrix\":" << s.matrix << ",\"maxNumReorderFrames\":" << s.reorder << "}\n";
}
// BT.601 limited range is what bgraToNv12 produces and the client applies. The
// MFT path declares nothing (the client treats "unspecified" as BT.601 limited,
// remote_decoder.cpp); x264 declares it explicitly (matrix 6, limited range).
bool declaresBt601Limited(const Sps& s) { return s.signal && !s.fullRange && s.colour && (s.matrix == 5 || s.matrix == 6); }
bool interpretedBt601Limited(const Sps& s) { return declaresBt601Limited(s) || (!s.fullRange && (!s.colour || s.matrix == 2)); }

// ------------------------------------------------------------ checks
// 1. Contract: the production preset and a High/CABAC variant (and the MFT
// reference) on the real decoder: profile, entropy coder, no B-frames or
// reordering, geometry, colour interpretation and decoded quality.
double contract(const char* name, Loop& loop, int expectedProfile, bool cabac, bool explicitColour) {
  constexpr int width = 1920, height = 1080;
  Scroll scroll(width, height, 3);
  loop.scoreColour = true;
  for (int n = 0; n < 90; ++n) loop.encode(n < 30 ? scroll.at(0) : scroll.at(n - 30), tickUs(n), n % 3 == 0);
  loop.finish();
  const auto tuning = loop.encoder.reportedTuning();
  require(loop.decoded == 90 && loop.encoded == 90, std::string(name) + ": not every frame decoded");
  require(loop.shape.idr == 1 && loop.shape.idrIndices.front() == 0 && loop.shape.idrWithHeaders == 1, std::string(name) + ": IDR placement");
  require(loop.shape.bSlices == 0, std::string(name) + ": B slices");
  require(!loop.shape.sps.empty(), std::string(name) + ": no SPS");
  const auto& s = loop.shape.sps.front(); printSps(name, s);
  require(s.profile == expectedProfile && (tuning.cabac == cabac), std::string(name) + ": profile/entropy coder");
  require(s.width == width && s.height == height, std::string(name) + ": SPS geometry");
  require(explicitColour ? declaresBt601Limited(s) : interpretedBt601Limited(s), std::string(name) + ": colour declaration is not BT.601 limited range");
  require(s.reorder <= 0, std::string(name) + ": reordering allowed");
  const double still = loop.meanQuality(0, tickUs(30)), moving = loop.meanQuality(tickUs(30), tickUs(90));
  double colour = 0; for (const auto& [pts, value] : loop.colour) if (pts < tickUs(30)) colour = value;  // last still frame
  std::cout << "{\"kind\":\"contract\",\"stream\":\"" << name << "\",\"decoded\":" << loop.decoded << ",\"idrFrames\":" << loop.shape.idr << ",\"cabac\":" << (tuning.cabac ? "true" : "false")
            << ",\"stillPsnrDb\":" << still << ",\"stillColourBlockPsnrDb\":" << colour << ",\"scrollPsnrDb\":" << moving << ",\"maxFrameBytes\":" << loop.shape.maxBytes << "}\n";
  // A 601/709 or range mismatch costs well over 10 dB on these blocks.
  require(still >= 35 && moving >= 28 && colour >= 38, std::string(name) + ": decoded quality/colour too low (wrong colour interpretation or broken stream)");
  return colour;
}

// 2. IDR placement: first frame, then only every fps*60 frames; a page switch
// (scene cut) never inserts one. Compared with the MFT path at the same GOP.
void idrPlacement() {
  constexpr int width = 640, height = 360, rate = 1;  // GOP = 60 frames
  const auto a = page(width, height, 1), b = page(width, height, 2);
  std::vector<size_t> found[2];
  for (int k = 0; k < 2; ++k) {
    Loop loop(width, height, 2000000, k ? host::EncoderBackend::mft : host::EncoderBackend::x264, rate);
    for (int n = 0; n < 130; ++n) loop.encode((n / 7) % 2 ? b : a, tickUs(n, rate), false);
    loop.finish();
    require(loop.decoded == 130, "IDR placement stream did not decode completely");
    found[k] = loop.shape.idrIndices;
  }
  const auto list = [](const std::vector<size_t>& v) { std::string s = "["; for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]); return s + "]"; };
  std::cout << "{\"kind\":\"idr-placement\",\"gopFrames\":60,\"frames\":130,\"pageSwitchEvery\":7,\"x264IdrIndices\":" << list(found[0]) << ",\"mftIdrIndices\":" << list(found[1]) << "}\n";
  require(found[0] == std::vector<size_t>{0, 60, 120}, "x264 IDR placement differs from the stream start + GOP rule");
}

// 2b. Requested IDR: when the broker channel's send queue drops video it asks
// for an IDR. On both backends the next frame is an IDR with SPS/PPS, and of
// one encoded stream a client decoder that lost the 8 frames before it shows
// every frame from it on exactly (byte for byte) like one that lost nothing.
void requestedKeyFrame() {
  constexpr int width = 640, height = 360;
  const auto a = page(width, height, 1), b = page(width, height, 2);
  for (int k = 0; k < 2; ++k) {
    const char* name = k ? "mft" : "x264";
    host::Encoder encoder(width, height, fps, 4000000, {}, k ? host::EncoderBackend::mft : host::EncoderBackend::x264);
    multitap::RemoteDecoder whole, lossy;
    host::check(whole.Initialize(UINT(width), UINT(height)), "Initialize actual client decoder");
    host::check(lossy.Initialize(UINT(width), UINT(height)), "Initialize actual client decoder");
    std::map<uint64_t, std::vector<uint8_t>> wholeFrames, lossyFrames;
    StreamShape shape; size_t index = 0;
    for (int n = 0; n < 40; ++n) {
      if (n == 20) encoder.requestKeyFrame();
      const auto& source = (n / 5) % 2 ? b : a;
      encoder.encode(source.data(), tickUs(n), [&](host::Frame frame) {
        inspect(frame, index++, shape);
        host::check(whole.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& shown) { wholeFrames[shown.pts_us] = std::move(shown.rgba); }), "Actual client decode");
        if (n < 12 || n >= 20) host::check(lossy.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& shown) { lossyFrames[shown.pts_us] = std::move(shown.rgba); }), "Actual client decode");
      });
    }
    host::check(whole.Drain([&](multitap::DecodedFrame&& shown) { wholeFrames[shown.pts_us] = std::move(shown.rgba); }), "Drain client decoder");
    host::check(lossy.Drain([&](multitap::DecodedFrame&& shown) { lossyFrames[shown.pts_us] = std::move(shown.rgba); }), "Drain client decoder");
    require(shape.idrIndices == std::vector<size_t>{0, 20} && shape.idrWithHeaders == 2, std::string(name) + ": requested IDR was not the next frame (with SPS/PPS)");
    require(lossyFrames.size() == 32, std::string(name) + ": frames after the requested IDR did not all decode");
    size_t identical = 0;
    for (int n = 20; n < 40; ++n) {
      const auto shown = lossyFrames.find(tickUs(n)), reference = wholeFrames.find(tickUs(n));
      if (shown != lossyFrames.end() && reference != wholeFrames.end() && shown->second == reference->second) ++identical;
    }
    std::cout << "{\"kind\":\"requested-idr\",\"encoder\":\"" << name << "\",\"idrIndices\":[0,20],\"lostBefore\":8,\"decodedAfter\":20,\"identicalToNoLoss\":" << identical << "}\n";
    require(identical == 20, std::string(name) + ": decoding after the requested IDR differs from a decoder that lost nothing");
  }
}

// 2b. udp-media-v1 LOSS (reference_history.hpp): frames after the client's last good
// one never reach its decoder; recover(lastGood) makes the next frame a P-frame coded
// only from that one or older. Of one encoded stream, a decoder that lost the frames
// in between shows every frame from the recovery frame on exactly (byte for byte) like
// one that lost nothing. Where the last good frame is gone (slid out of the DPB, or
// before an IDR the client lost) recover() refuses and the next frame is an IDR.
// Sizes: recovery frame against the P-frame and the IDR at the same point.
struct RecoveryCase { const char* name; int lostFrom, recoverAt, secondLostFrom = -1, secondRecoverAt = -1; bool lostIdr = false, expectRecovered = true; };
void referenceRecovery() {
  constexpr int width = 1280, height = 720, frames = 60;
  Scroll scroll(width, height, 5);
  const auto content = [&](int n, bool scrolling) -> std::vector<uint8_t> { return scrolling ? scroll.at(n) : grain(width, height, n); };
  for (const bool scrolling : {true, false}) {
    // A reference run: the same frames with nothing lost, and with an IDR at the recovery point.
    const auto sizesWith = [&](int idrAt) {
      host::Encoder encoder(width, height, fps, 8000000, host::X264Settings::production()); std::vector<size_t> sizes;
      for (int n = 0; n < frames; ++n) { if (n == idrAt) encoder.requestKeyFrame(); const auto source = content(n, scrolling); encoder.encode(source.data(), tickUs(n), [&](host::Frame f) { sizes.push_back(f.bytes.size()); }); }
      return sizes;
    };
    const auto plain = sizesWith(-1);
    for (const RecoveryCase& c : {RecoveryCase{"one-lost", 20, 21}, RecoveryCase{"8-lost", 20, 28}, RecoveryCase{"15-lost-dpb-edge", 20, 35}, RecoveryCase{"twice", 20, 24, 30, 33},
                                  RecoveryCase{"16-lost-past-dpb", 20, 36, -1, -1, false, false}, RecoveryCase{"idr-lost", 20, 25, -1, -1, true, false}}) {
      host::Encoder encoder(width, height, fps, 8000000, host::X264Settings::production());
      multitap::RemoteDecoder whole, lossy;
      host::check(whole.Initialize(UINT(width), UINT(height)), "Initialize actual client decoder");
      host::check(lossy.Initialize(UINT(width), UINT(height)), "Initialize actual client decoder");
      std::map<uint64_t, std::vector<uint8_t>> wholeFrames, lossyFrames; std::vector<host::Frame> stream; size_t lateOutputs = 0;
      // Frame 5 is lost too: the stream starts with x264's single reference, so that first
      // LOSS is refused and its IDR (frame 6) reopens the encoder with the larger DPB.
      const auto lost = [&](int n) { return n == 5 || (n >= c.lostFrom && n < c.recoverAt) || (n >= c.secondLostFrom && n < c.secondRecoverAt); };
      for (int n = 0; n < frames; ++n) {
        if (n == 6) { require(!encoder.recover(tickUs(4)), std::string(c.name) + ": a single-reference stream recovered"); encoder.requestKeyFrame(); }
        if (c.lostIdr && n == c.lostFrom) encoder.requestKeyFrame();
        if (n == c.recoverAt || n == c.secondRecoverAt) {
          const bool recovered = encoder.recover(tickUs((n == c.recoverAt ? c.lostFrom : c.secondLostFrom) - 1));
          require(recovered == c.expectRecovered, std::string(c.name) + ": recover() " + (recovered ? "accepted" : "refused") + " against the DPB rule");
          if (!recovered) encoder.requestKeyFrame();
        }
        const auto source = content(n, scrolling);
        encoder.encode(source.data(), tickUs(n), [&](host::Frame frame) {
          size_t shown = 0;
          host::check(whole.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& out) { ++shown; wholeFrames[out.pts_us] = std::move(out.rgba); }), "Actual client decode");
          if (n > 0 && shown != 1) ++lateOutputs;  // low latency: each frame out of its own Decode call
          if (!lost(n)) host::check(lossy.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& out) { lossyFrames[out.pts_us] = std::move(out.rgba); }), "Actual client decode");
          stream.push_back(std::move(frame));
        });
      }
      host::check(whole.Drain([&](multitap::DecodedFrame&& out) { wholeFrames[out.pts_us] = std::move(out.rgba); }), "Drain client decoder");
      host::check(lossy.Drain([&](multitap::DecodedFrame&& out) { lossyFrames[out.pts_us] = std::move(out.rgba); }), "Drain client decoder");
      require(stream[size_t(c.recoverAt)].key == !c.expectRecovered, std::string(c.name) + ": the frame after recover() has the wrong type");
      require(stream[6].key && sps(stream[6]).refs == host::recoveryReferenceFrames(width, height) && sps(stream[0]).refs == 1, std::string(c.name) + ": the first LOSS did not widen the DPB at its IDR");
      size_t identical = 0, expected = 0;
      for (int n = 6; n < c.recoverAt; ++n) if (!lost(n)) { ++expected; const auto a = lossyFrames.find(tickUs(n)), b = wholeFrames.find(tickUs(n)); identical += a != lossyFrames.end() && b != wholeFrames.end() && a->second == b->second; }
      for (int n = c.recoverAt; n < frames; ++n) {
        if (lost(n)) continue;
        ++expected;
        const auto shown = lossyFrames.find(tickUs(n)), reference = wholeFrames.find(tickUs(n));
        if (shown != lossyFrames.end() && reference != wholeFrames.end() && shown->second == reference->second) ++identical;
      }
      const auto idr = sizesWith(c.recoverAt);
      std::cout << "{\"kind\":\"x264-reference-recovery\",\"content\":\"" << (scrolling ? "scroll-8px" : "grain") << "\",\"case\":\"" << c.name << "\",\"lost\":" << (c.recoverAt - c.lostFrom)
                << ",\"recovered\":" << (c.expectRecovered ? "true" : "false") << ",\"identicalToNoLoss\":" << identical << ",\"expected\":" << expected
                << ",\"answerBytes\":" << stream[size_t(c.recoverAt)].bytes.size() << ",\"pFrameBytes\":" << plain[size_t(c.recoverAt)] << ",\"idrBytes\":" << idr[size_t(c.recoverAt)]
                << ",\"lateDecoderOutputs\":" << lateOutputs << "}\n";
      require(identical == expected, std::string(c.name) + ": decoding after the recovery frame differs from a decoder that lost nothing");
      require(!lateOutputs, std::string(c.name) + ": the decoder held frames back with the larger DPB");
    }
  }
}

// 2c. What a larger DPB costs: private memory and steady encode time (40 frames to warm
// up, then 120 timed), against x264's own single reference, three alternating runs each,
// median. Only sessions whose client sent LOSS pay it. THIS PC only, under whatever else
// runs on it.
void referenceCost() {
  for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{3840, 2160}}) {
    Scroll scroll(width, height, 7);
    const int wide = host::recoveryReferenceFrames(width, height);
    std::vector<int> sizes{1, 4, wide}; sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    std::map<int, std::vector<double>> times; std::map<int, double> megabytes;
    for (int run = 0; run < 3; ++run) for (size_t k = 0; k < sizes.size(); ++k) {
      const int frames = sizes[run % 2 ? sizes.size() - 1 - k : k];
      auto settings = host::X264Settings::production(); settings.referenceFrames = frames;
      PROCESS_MEMORY_COUNTERS_EX before{}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
      host::Encoder encoder(width, height, fps, 20000000, settings);
      for (int n = 0; n < 40; ++n) { const auto& source = scroll.at(n); encoder.encode(source.data(), tickUs(n), [](host::Frame) {}); }
      const auto began = std::chrono::steady_clock::now();
      for (int n = 40; n < 160; ++n) { const auto& source = scroll.at(n); encoder.encode(source.data(), tickUs(n), [](host::Frame) {}); }
      times[frames].push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / 120);
      PROCESS_MEMORY_COUNTERS_EX after{}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
      megabytes[frames] = std::max(megabytes[frames], (double(after.PrivateUsage) - double(before.PrivateUsage)) / 1048576.0);
    }
    for (const int frames : sizes) {
      auto& t = times[frames]; std::sort(t.begin(), t.end());
      std::cout << "{\"kind\":\"x264-reference-cost\",\"width\":" << width << ",\"height\":" << height << ",\"referenceFrames\":" << frames
                << ",\"privateMb\":" << megabytes[frames] << ",\"encodeMsMedian\":" << t[1] << ",\"thisPcOnly\":true}\n";
    }
  }
}

// 3. Live bitrate change: no IDR, every frame decodes, output bounded by the
// new VBV maxrate, quality follows the target.
void liveBitrate() {
  constexpr int width = 1920, height = 1080, phase = 90;
  const int targets[] = {20000000, 8000000, 40000000, 12000000};
  Loop loop(width, height, targets[0], host::EncoderBackend::x264);
  int n = 0;
  for (int p = 0; p < 4; ++p) {
    if (p) { require(loop.encoder.setBitrate(targets[p]), "x264 refused a live bitrate change"); require(loop.encoder.reportedBitrate() == targets[p], "Reported target differs after the change"); }
    for (int k = 0; k < phase; ++k, ++n) loop.encode(grain(width, height, n), tickUs(n), k >= phase - 10);
  }
  loop.finish();
  require(loop.decoded == size_t(4 * phase) && loop.shape.idr == 1, "A live bitrate change inserted an IDR or broke decoding");
  std::cout << "{\"kind\":\"x264-live-bitrate\",\"width\":1920,\"height\":1080,\"keyFrames\":" << loop.shape.idr << ",\"phases\":[";
  double quality[4]{};
  for (int p = 0; p < 4; ++p) {
    const uint64_t from = tickUs(p * phase + phase / 2), to = tickUs((p + 1) * phase);
    quality[p] = loop.meanQuality(tickUs(p * phase), tickUs((p + 1) * phase));
    const double mbps = loop.mbps(from, to);
    // VBV: any window carries at most one buffer plus maxrate x its duration.
    const auto rate = host::x264RateControl(width, height, fps, targets[p], host::X264Settings::production());
    const double seconds = double(to - from) / 1e6, bound = (rate.bufferKbit * 1000.0 + targets[p] * seconds) / seconds / 1e6;
    std::cout << (p ? "," : "") << "{\"targetMbps\":" << targets[p] / 1e6 << ",\"crf\":" << rate.crf << ",\"secondHalfMbps\":" << mbps << ",\"vbvBoundMbps\":" << bound << ",\"psnrDb\":" << quality[p] << "}";
    require(mbps <= bound * 1.05, "Output exceeded the live VBV maxrate/buffer");
  }
  std::cout << "]}\n";
  require(quality[2] >= quality[1] + 3 && quality[1] < quality[0], "Quality did not follow the live bitrate");
  require(!loop.encoder.setBitrate(targets[0]) || loop.encoder.bitrate() == targets[0], "Bitrate bookkeeping");
}

// 4. Resize epoch: a fresh encoder at the new geometry starts with an IDR and
// SPS/PPS that a fresh client decoder (one per epoch) accepts at once.
void resizeEpoch() {
  struct Size { int width, height; } sizes[] = {{1920, 1080}, {1264, 760}, {2560, 1440}};
  uint64_t pts = 0;
  for (const auto& size : sizes) {
    Loop loop(size.width, size.height, 20000000, host::EncoderBackend::automatic);
    require(loop.encoder.backend() == host::EncoderBackend::x264, "Automatic did not select x264");
    const auto content = page(size.width, size.height, size.width);
    for (int n = 0; n < 12; ++n, pts += 16667) loop.encode(content, pts, n == 11);
    loop.finish();
    require(loop.decoded == 12 && loop.shape.idr == 1 && loop.shape.idrIndices.front() == 0 && loop.shape.idrWithHeaders == 1, "Epoch did not start with a decodable IDR");
    require(loop.shape.sps.front().width == size.width && loop.shape.sps.front().height == size.height, "Epoch SPS geometry");
    std::cout << "{\"kind\":\"x264-epoch\",\"width\":" << size.width << ",\"height\":" << size.height << ",\"firstFrameIdr\":true,\"lastPsnrDb\":" << loop.meanQuality(0, pts) << "}\n";
  }
}

// 5. VBV bound: the 4K worst cases stay inside the 4 MiB media packet bound
// (the MFT path codes a 4K page change as ~4.9 MB, docs/adaptive-bitrate.md).
void vbvBound() {
  constexpr int width = 3840, height = 2160;
  const auto a = page(width, height, 1), b = page(width, height, 2), c = noise(width, height, 7), d = noise(width, height, 99);
  for (const int bitrate : {20000000, 100000000}) {
    Loop loop(width, height, bitrate, host::EncoderBackend::x264);
    host::FrameEncodePolicy policy(size_t(width) * height * 3 / 2, host::FrameRefinement::forCbr(bitrate, fps));
    size_t pageMax = 0, noiseMax = 0, submitted = 0;
    for (int n = 0; n < 150; ++n) {
      // 0-59 page A (IDR + refinement), 60-99 page B, 100 noise, 101 other noise, then B.
      const auto& frame = n < 60 ? a : n < 100 ? b : n == 100 ? c : n == 101 ? d : b;
      const auto decision = policy.prepare(frame, tickUs(n)); if (!decision.submit) continue;
      ++submitted; loop.encode(frame, tickUs(n), false);
      require(policy.output(tickUs(n), loop.lastKey, tickUs(n), loop.lastBytes), "Policy rejected an x264 output");
      auto& largest = n == 100 || n == 101 ? noiseMax : pageMax; largest = std::max(largest, loop.lastBytes);
    }
    loop.finish();
    const auto rate = host::x264RateControl(width, height, fps, bitrate, host::X264Settings::production());
    std::cout << "{\"kind\":\"x264-vbv-bound\",\"width\":3840,\"height\":2160,\"targetMbps\":" << bitrate / 1e6 << ",\"vbvBufferKbit\":" << rate.bufferKbit << ",\"crf\":" << rate.crf
              << ",\"submitted\":" << submitted << ",\"maxPageFrameBytes\":" << pageMax << ",\"maxNoiseFrameBytes\":" << noiseMax << ",\"mediaFrameLimit\":" << host::encoderMaximumFrameBytes << "}\n";
    require(loop.decoded == loop.encoded && loop.shape.idr == 1, "4K stream did not decode completely");
    require(std::max(pageMax, noiseMax) <= size_t(rate.bufferKbit) * 1000 / 8 * 11 / 10, "A frame exceeded the VBV buffer");
    require(std::max(pageMax, noiseMax) <= host::encoderMaximumFrameBytes, "A frame exceeded the media packet bound");
  }
}

// 6. Automatic fallback to the MFT: open failure and a mid-stream encode
// failure, observed once with the fixed code; the client keeps decoding.
void fallback() {
  constexpr int width = 1280, height = 720;
  std::vector<uint32_t> observed; host::setEncoderFallbackObserver([&](uint32_t code) { observed.push_back(code); });
  Scroll scroll(width, height, 5);
  {
    host::x264testing::failOpenCode = host::encoderFallbackX264Open;
    Loop loop(width, height, 8000000, host::EncoderBackend::automatic);
    host::x264testing::failOpenCode = 0;
    require(loop.encoder.backend() == host::EncoderBackend::mft && std::string(loop.encoder.backendName()) == "mft" && loop.encoder.fallbackCode() == host::encoderFallbackX264Open, "Open failure did not fall back to the MFT");
    for (int n = 0; n < 20; ++n) loop.encode(scroll.at(n), tickUs(n), n == 19);
    loop.finish();
    require(loop.decoded == 20 && loop.shape.idr == 1, "MFT fallback stream did not decode");
    require(observed == std::vector<uint32_t>{host::encoderFallbackX264Open}, "Open fallback was not observed exactly once");
  }
  observed.clear();
  {
    host::x264testing::failEncodeAtFrame = 30; host::x264testing::failEncodeCode = host::encoderFallbackX264Encode;
    Loop loop(width, height, 8000000, host::EncoderBackend::automatic);
    require(loop.encoder.backend() == host::EncoderBackend::x264, "Automatic did not start on x264");
    for (int n = 0; n < 60; ++n) {
      loop.encode(scroll.at(n), tickUs(n), n % 5 == 4);
      require(std::string(loop.encoder.backendName()) == (n < 30 ? "x264" : "mft"), "Backend name does not follow the fallback");
    }
    host::x264testing::failEncodeAtFrame = -1;
    loop.finish();
    require(loop.encoder.fallbackCode() == host::encoderFallbackX264Encode && observed == std::vector<uint32_t>{host::encoderFallbackX264Encode}, "Mid-stream fallback code/observation");
    require(loop.shape.idrIndices == std::vector<size_t>{0, 30} && loop.shape.idrWithHeaders == 2, "Fallback did not restart with an IDR at the failed frame");
    require(loop.decoded == 60, "The client decoder did not continue across the fallback");
    const double before = loop.meanQuality(0, tickUs(30)), after = loop.meanQuality(tickUs(30), tickUs(60));
    std::cout << "{\"kind\":\"x264-fallback\",\"openFailureCode\":2,\"midStreamFailureCode\":3,\"idrIndices\":[0,30],\"decoded\":60,\"x264PsnrDb\":" << before << ",\"mftPsnrDb\":" << after << "}\n";
    require(before >= 28 && after >= 28, "Decoded quality across the fallback");
  }
  {
    host::x264testing::failEncodeAtFrame = 2;
    host::Encoder strict(width, height, fps, 8000000, {}, host::EncoderBackend::x264);
    const auto frame = scroll.at(0); bool threw = false; size_t outputs = 0;
    try { for (int n = 0; n < 4; ++n) strict.encode(frame.data(), tickUs(n), [&](host::Frame) { ++outputs; }); } catch (const std::exception&) { threw = true; }
    host::x264testing::failEncodeAtFrame = -1;
    require(threw && outputs == 2 && strict.backend() == host::EncoderBackend::x264, "Explicit x264 swallowed a failure");
  }
  host::setEncoderFallbackObserver({});
  // An explicit MFT tuning never selects x264.
  host::Encoder tuned(width, height, fps, 8000000, {eAVEncH264VProfile_Main, -1, false});
  require(tuned.backend() == host::EncoderBackend::mft, "A Microsoft MFT tuning selected x264");
}

// 7. Unchanged macroblocks (x264 mb_info): the flags match a per-pixel
// comparison on every size, including partial edge macroblocks; and against
// the same encoder without the hint, a typed line, a page switch at a low
// target that refinement re-submits, and a requested IDR all decode with the
// same quality. Its CPU saving is measured by MultitapHostEncoderBenchmark
// (enc=x264/constant=0 against enc=x264); this loop decodes inside encode().
void constantBlocks() {
  std::mt19937 random(5);
  size_t checkedBlocks = 0;
  for (const auto [width, height] : std::vector<std::pair<int, int>>{{16, 16}, {18, 10}, {34, 34}, {640, 360}, {1366, 768}, {1920, 1080}}) {
    const size_t bytes = size_t(width) * height * 3 / 2, columns = size_t(width + 15) / 16, rows = size_t(height + 15) / 16;
    std::vector<uint8_t> previous(bytes), current(bytes), flags(columns * rows, 0x55);
    for (auto& value : previous) value = uint8_t(random());
    for (int round = 0; round < 12; ++round) {
      current = previous;
      // A few changed rectangles (luma and/or chroma), sometimes none.
      for (int k = 0; k < round % 4; ++k) {
        const int x = int(random() % width), y = int(random() % height), w = 1 + int(random() % 40), h = 1 + int(random() % 20);
        for (int yy = y; yy < std::min(height, y + h); ++yy) for (int xx = x; xx < std::min(width, x + w); ++xx) {
          if (random() % 2) current[size_t(yy) * width + xx] ^= uint8_t(1 + random() % 255);
          else current[size_t(width) * height + size_t(yy / 2) * width + xx] ^= uint8_t(1 + random() % 255);
        }
      }
      std::vector<uint8_t> expected(columns * rows, 1);
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) if (current[size_t(y) * width + x] != previous[size_t(y) * width + x]) expected[size_t(y / 16) * columns + x / 16] = 0;
      for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width; ++x) { const size_t at = size_t(width) * height + size_t(y) * width + x; if (current[at] != previous[at]) expected[size_t(y / 8) * columns + x / 16] = 0; }
      const auto before = current;
      const size_t constant = host::markConstantMacroblocks(current.data(), previous.data(), width, height, flags.data());
      require(flags == expected && constant == size_t(std::count(expected.begin(), expected.end(), uint8_t(1))), "Constant macroblock flags differ from a per-pixel comparison");
      require(previous == current && current == before, "The previous picture was not updated exactly");
      checkedBlocks += flags.size();
    }
  }
  constexpr int width = 1920, height = 1080;
  const auto a = page(width, height, 1), b = page(width, height, 2);
  struct Result { double typing = 0, refined = 0, afterIdr = 0; size_t bytes = 0; };
  Result results[2];
  for (int hint = 0; hint < 2; ++hint) {
    // One thread: x264 is then deterministic (sliced-thread VBV moved these
    // values by up to 0.45 dB between identical runs), so only the hint differs.
    auto settings = host::X264Settings::production(); settings.constantBlocks = hint == 1; settings.threads = 1;
    Loop loop(width, height, 8000000, settings);
    std::vector<uint8_t> frame = a;
    for (int n = 0; n < 150; ++n) {
      if (n < 60) { const int x = 40 + 9 * (n / 2), y = 300; for (int yy = y; yy < y + 16; ++yy) for (int xx = x; xx < x + 7; ++xx) if ((xx + yy + n) % 3) frame[size_t(yy) * width + xx] = 30; }  // typing
      else if (n == 60) frame = b;  // page switch, then the same picture re-submitted (refinement)
      if (n == 120) loop.encoder.requestKeyFrame();
      loop.encode(frame, tickUs(n), n == 59 || n == 119 || n == 149);
    }
    loop.finish();
    require(loop.decoded == 150 && loop.encoded == 150, "Constant-block stream did not decode completely");
    require(loop.shape.idrIndices == std::vector<size_t>{0, 120} && loop.shape.idrWithHeaders == 2, "Constant blocks changed IDR placement or the requested IDR");
    auto& r = results[hint]; r.typing = loop.quality.at(tickUs(59)); r.refined = loop.quality.at(tickUs(119)); r.afterIdr = loop.quality.at(tickUs(149));
    for (const auto& [pts, size] : loop.sizes) r.bytes += size;
  }
  std::cout << "{\"kind\":\"x264-constant-blocks\",\"checkedBlocks\":" << checkedBlocks;
  for (int hint = 0; hint < 2; ++hint) std::cout << ",\"" << (hint ? "hint" : "none") << "\":{\"typingPsnrDb\":" << results[hint].typing << ",\"refinedPagePsnrDb\":" << results[hint].refined << ",\"afterIdrPsnrDb\":" << results[hint].afterIdr << ",\"bytes\":" << results[hint].bytes << "}";
  std::cout << "}\n";
  require(results[1].typing >= results[0].typing - 0.5 && results[1].refined >= results[0].refined - 0.5 && results[1].afterIdr >= results[0].afterIdr - 0.5, "Constant blocks lowered the decoded quality");
  require(results[1].refined >= 35, "Refinement no longer sharpens an unchanged page with constant blocks");
}

void configuration() {
  using host::EncoderBackend;
  require(host::encoderBackendFromConfig(R"({"videoEncoder":"mft"})") == EncoderBackend::mft, "encoder.json mft");
  require(host::encoderBackendFromConfig(R"({"videoEncoder":"x264"})") == EncoderBackend::x264, "encoder.json x264");
  require(host::encoderBackendFromConfig("{}") == EncoderBackend::x264 && host::encoderBackendFromConfig("not json") == EncoderBackend::x264 && host::encoderBackendFromConfig(R"({"videoEncoder":"MFT"})") == EncoderBackend::x264, "encoder.json defaults to x264");
  require(host::x264EncoderAvailable(), "x264 not built in");
  bool gpuRefused = false;
  try { host::Encoder denied(320, 180, fps, 4000000, {}, EncoderBackend::nvenc); } catch (const std::runtime_error& error) {
    gpuRefused = std::string(error.what()) == "GPU opening is prohibited by the CPU test configuration";
  }
  require(gpuRefused, "Isolated CPU fixture allowed an actual GPU request");
}
}

int main() {
  try {
    CpuConfigurationScope configurationScope;
    host::Runtime runtime;
    std::cout << std::fixed << std::setprecision(2);
    configuration();
    {
      // The colour check must see a matrix or range mismatch at all.
      const auto source = page(1920, 1080, 1); std::vector<uint8_t> wrong(size_t(1920) * 1080 * 4);
      multitap::video::nv12ToRgba(source.data(), wrong.data(), 1920, 1080, 1920, 1080, true, false);
      const double matrix = colourPsnr(1920, 1080, source, wrong);
      multitap::video::nv12ToRgba(source.data(), wrong.data(), 1920, 1080, 1920, 1080, false, true);
      const double range = colourPsnr(1920, 1080, source, wrong);
      std::cout << "{\"kind\":\"colour-check-sensitivity\",\"bt709MisreadPsnrDb\":" << matrix << ",\"fullRangeMisreadPsnrDb\":" << range << "}\n";
      require(matrix < 32 && range < 32, "The colour check cannot detect a misread stream");
    }
    {
      // The MFT path is the reference the released clients already decode.
      Loop mft(1920, 1080, 20000000, host::EncoderBackend::mft);
      const double reference = contract("mft-production", mft, 66, false, false);
      Loop production(1920, 1080, 20000000, host::X264Settings::production());
      const double colour = contract("x264-production", production, 66, false, true);
      host::X264Settings high; high.preset = "superfast"; high.profile = "high";
      Loop highCabac(1920, 1080, 20000000, high);
      contract("x264-superfast-high-cabac", highCabac, 100, true, true);
      // The SSE-only code paths a VM with Proxmox's default x86-64-v2 CPU type runs.
      auto v2 = host::X264Settings::production(); v2.cpu = "x86-64-v2";
      Loop sse(1920, 1080, 20000000, v2);
      contract("x264-production-x86-64-v2", sse, 66, false, true);
      require(colour >= reference - 3, "x264 colour blocks decode worse than the MFT path");
    }
    idrPlacement();
    requestedKeyFrame();
    constantBlocks();
    referenceRecovery();
    referenceCost();
    liveBitrate();
    resizeEpoch();
    vbvBound();
    fallback();
    require(configurationProviderCalls.load() > 0, "Automatic CPU paths did not use the isolated configuration provider");
    std::cout << "{\"kind\":\"x264-encoder-summary\",\"passed\":true,\"actualX264\":true,\"actualClientDecoder\":true,\"syntheticPixels\":true,\"desktopCaptured\":false,\"relayUsed\":false,\"realSessionAcceptance\":false,\"isolatedConfiguration\":true,\"configurationProviderCalls\":" << configurationProviderCalls.load() << ",\"settingsRead\":false,\"actualGpu\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
