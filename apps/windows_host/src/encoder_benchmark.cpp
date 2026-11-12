// Production-path encoder benchmark (docs/x264-encoder.md): host::Encoder (x264
// or the Microsoft MFT, plus explicit x264 variants for the preset/rate-control
// choice) driven by the production FrameEncodePolicy with refinement, on a 60 Hz
// clock, every output decoded by the client's actual RemoteDecoder. Scenes,
// ink metric and link model follow research/x264-eval-claude so the numbers are
// comparable. Generated scenes only: no desktop, file or screen is read. A pass
// here is NOT a real-session, relay, latency or user-acceptance result.
//
//   MultitapHostEncoderBenchmark NEW_OUTPUT_DIR [cores=N] [sizes=760,1080,1440,5120x1440]
//       [rates=8,20,40,auto] [scenes=scroll,map,page,typing,video] [frames=N] [nopace] [idle]
//       [tiles] [idr=N] [enc=mft] [enc=x264] [enc=x264/preset=superfast/profile=high/crf=14/buffer=500/threads=4]
//       [enc=x264/threads=4/x.lookahead-threads=1]  (x.NAME=VALUE: x264_param_parse, benchmarks only)
//       [enc=x264/constant=0]  (without the unchanged-macroblock hint)
//       [enc=x264/kernel=sse2]  (the BGRA->NV12 conversion without AVX2)
#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "lossless_tile_source.hpp"
#include "bitrate_controller.hpp"
#include "remote_decoder.h"
#include "../../../native/windows_video/color.hpp"
#include "../../../native/windows_video/bgra_nv12_sse2.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>

namespace fs = std::filesystem;
using host::Clock;
namespace {
constexpr int fps = 60;
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); }
double millis(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
double percentile(std::vector<double> v, double f) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); size_t i = size_t(std::ceil(f * double(v.size()))); return v[std::max<size_t>(i, 1) - 1]; }
double mean(const std::vector<double>& v) { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()); }
double psnr(double mse) { return mse < 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse); }
uint64_t tickUs(int n) { return uint64_t(n) * 1000000 / fps; }
// Query*CycleTime counts time-stamp-counter ticks; the TSC rate against the wall
// clock does not depend on this thread being scheduled (a busy-loop calibration
// read 5x too low at IDLE priority on a loaded machine).
double cyclesPerMs() {
  const auto t0 = Clock::now(); const uint64_t r0 = __rdtsc();
  Sleep(250);
  const uint64_t r1 = __rdtsc(); return double(r1 - r0) / millis(Clock::now() - t0);
}
ULONG64 processCycles() { ULONG64 c = 0; QueryProcessCycleTime(GetCurrentProcess(), &c); return c; }

// ------------------------------------------------------------ drawing (research/x264-eval-claude)
struct Lcg { uint32_t s; explicit Lcg(uint32_t seed) : s(seed) {} uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; } int r(int n) { return n <= 0 ? 0 : int(next() % uint32_t(n)); } };
struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool empty() const { return x1 <= x0 || y1 <= y0; } };
struct Canvas {
  int w, h; HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; uint8_t* px = nullptr;
  Canvas(int W, int H, COLORREF bg) : w(W), h(H) {
    dc = CreateCompatibleDC(nullptr); BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0); require(dc && bmp && px, "Cannot create GDI canvas");
    old = SelectObject(dc, bmp); fill(Rect{0, 0, W, H}, bg); SetBkMode(dc, TRANSPARENT);
  }
  ~Canvas() { if (dc && old) SelectObject(dc, old); if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); }
  Canvas(const Canvas&) = delete; Canvas& operator=(const Canvas&) = delete;
  void fill(Rect r, COLORREF c) { RECT rc{r.x0, r.y0, r.x1, r.y1}; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &rc, b); DeleteObject(b); }
  void hline(int x0, int x1, int y, COLORREF c) { fill(Rect{x0, y, x1, y + 1}, c); }
  void vline(int x, int y0, int y1, COLORREF c) { fill(Rect{x, y0, x + 1, y1}, c); }
  const uint8_t* row(int y) const { return px + size_t(y) * w * 4; }
};
struct Fonts {
  std::map<int, HFONT> cache;
  HFONT get(int size, int family, bool bold) {
    const int key = size * 4 + family * 2 + (bold ? 1 : 0); auto it = cache.find(key); if (it != cache.end()) return it->second;
    HFONT f = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family ? L"Segoe UI" : L"Malgun Gothic");
    require(f != nullptr, "Cannot create font"); cache[key] = f; return f;
  }
  ~Fonts() { for (auto& [k, f] : cache) DeleteObject(f); }
};
const wchar_t* kSentences[] = {
  L"가상 데스크톱의 글자가 원래 화면처럼 또렷하게 보이는지 확인합니다.", L"The quick brown fox jumps over the lazy dog. 0123456789",
  L"설정 > 시스템 > 디스플레이 > 해상도 2560 × 1440 · 배율 100%", L"파일(F)  편집(E)  보기(V)  삽입(I)  서식(O)  도구(T)  도움말(H)",
  L"Invoice #48213 · 합계 ₩1,234,500 · 2026-10-25 14:32:07 KST", L"원격 세션 품질: 스크롤, 지도 이동, 문서 전환에서 선명도를 측정",
  L"SELECT id, name, updated_at FROM desktops WHERE owner = ?;", L"회의록 — 3분기 예산 검토, 담당자 확인, 다음 일정 11월 4일(화)",
  L"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do", L"고객 지원 요청 #7731: 프린터 연결 문제 — 상태: 처리 중",
  L"https://intranet.example.local/docs/handbook/section-4.2#remote", L"한글 맞춤법 검사 · 띄어쓰기 · 문장 부호 · 줄 간격 1.5",
  L"Mon Tue Wed Thu Fri Sat Sun | 09:00 10:30 13:00 15:45 17:20", L"주의: 저장하지 않은 변경 내용은 창을 닫으면 사라집니다.",
  L"git commit -m \"Fix scroll stutter in viewer\" --author=dev", L"표 1. 지역별 매출 (단위: 백만 원)  서울 1,240  부산 860  대구 512",
};
constexpr int kSentenceCount = int(sizeof(kSentences) / sizeof(kSentences[0]));
void text(Canvas& c, HFONT f, COLORREF color, int x, int y, Rect clip, const std::wstring& s) {
  const auto old = SelectObject(c.dc, f); SetTextColor(c.dc, color); RECT rc{clip.x0, clip.y0, clip.x1, clip.y1};
  ExtTextOutW(c.dc, x, y, ETO_CLIPPED, &rc, s.c_str(), UINT(s.size()), nullptr); SelectObject(c.dc, old);
}
void drawDocument(Canvas& c, Fonts& fonts, Rect r, int seed) {
  int y = r.y0 + 10, line = 0; const Rect clip{r.x0 + 16, r.y0, r.x1 - 16, r.y1};
  while (y + 26 < r.y1) {
    const int kind = (line + seed) % 14;
    if (kind == 0) {
      const std::wstring t = L"제" + std::to_wstring(line / 14 + 1) + L"장 · Section " + std::to_wstring(seed) + L"." + std::to_wstring(line) + L"  원격 데스크톱 운영 안내";
      text(c, fonts.get(20, 0, true), RGB(20, 20, 24), r.x0 + 24, y, clip, t); y += 30; c.hline(r.x0 + 24, r.x1 - 24, y, RGB(200, 200, 200)); y += 8;
    } else if (kind == 7) {
      const int cells = 5, cw = (r.x1 - r.x0 - 48) / cells;
      for (int k = 0; k < cells; ++k) {
        const int x = r.x0 + 24 + k * cw;
        c.hline(x, x + cw + 1, y, RGB(190, 190, 190)); c.hline(x, x + cw + 1, y + 22, RGB(190, 190, 190)); c.vline(x, y, y + 23, RGB(190, 190, 190)); c.vline(x + cw, y, y + 23, RGB(190, 190, 190));
        const std::wstring t = (k == 0 ? std::wstring(L"항목 ") : std::wstring(L"")) + std::to_wstring((seed * 131 + line * 17 + k * 977) % 100000);
        text(c, fonts.get(12, 1, false), RGB(51, 51, 51), x + 6, y + 4, Rect{x + 1, y + 1, x + cw, y + 22}, t);
      }
      y += 30;
    } else {
      const int size = kind % 3 == 0 ? 12 : kind % 3 == 1 ? 14 : 16;
      const COLORREF color = kind == 3 ? RGB(11, 87, 208) : kind == 11 ? RGB(197, 34, 31) : kind == 5 ? RGB(95, 99, 104) : RGB(34, 34, 34);
      std::wstring t = std::to_wstring(line + 1) + L"  ";
      for (int k = 0; k < 4; ++k) t += std::wstring(kSentences[(line * 7 + seed * 5 + k * 3) % kSentenceCount]) + L"   ";
      t += L"· " + std::to_wstring((seed * 977 + line * 131) % 10000);
      text(c, fonts.get(size, kind % 2, false), color, r.x0 + 24, y, clip, t); y += size + 9;
    }
    ++line;
  }
}
void drawChrome(Canvas& c, Fonts& fonts, int seed) {
  c.fill(Rect{0, 0, c.w, 64}, RGB(243, 243, 243)); c.hline(0, c.w, 63, RGB(210, 210, 210));
  text(c, fonts.get(12, 1, false), RGB(32, 32, 32), 16, 8, Rect{0, 0, c.w, 30}, L"Multitap 문서 뷰어 — 보고서_" + std::to_wstring(seed) + L".docx   |   파일  편집  보기  도움말");
  c.fill(Rect{12, 34, c.w - 12, 58}, RGB(255, 255, 255)); c.hline(12, c.w - 12, 34, RGB(200, 200, 200)); c.hline(12, c.w - 12, 57, RGB(200, 200, 200));
  text(c, fonts.get(13, 1, false), RGB(60, 64, 67), 22, 38, Rect{12, 34, c.w - 12, 58}, L"https://intranet.example.local/reports/2026/q3/summary?page=" + std::to_wstring(seed));
}
const wchar_t* kPlaces[] = {L"서울역", L"시청", L"한강공원", L"중앙로", L"남산타워", L"종로 3가", L"Main St", L"River Rd", L"강남대로", L"문화센터", L"초등학교", L"Central Park", L"시립도서관", L"버스터미널", L"우체국", L"병원"};
void drawMap(Canvas& c, Fonts& fonts, uint32_t seed) {
  Lcg rng(seed);
  for (int k = 0; k < c.w * c.h / 30000; ++k) {
    const int x = rng.r(c.w), y = rng.r(c.h), rw = 80 + rng.r(320), rh = 60 + rng.r(260);
    HBRUSH b = CreateSolidBrush(k % 3 == 0 ? RGB(170, 211, 223) : RGB(205, 232, 190)); const auto ob = SelectObject(c.dc, b), op = SelectObject(c.dc, GetStockObject(NULL_PEN));
    Ellipse(c.dc, x, y, x + rw, y + rh); SelectObject(c.dc, ob); SelectObject(c.dc, op); DeleteObject(b);
  }
  { HBRUSH b = CreateSolidBrush(RGB(224, 220, 214)); HPEN p = CreatePen(PS_SOLID, 1, RGB(206, 200, 192)); const auto ob = SelectObject(c.dc, b), op = SelectObject(c.dc, p);
    for (int k = 0; k < c.w * c.h / 7000; ++k) { const int x = rng.r(c.w), y = rng.r(c.h); Rectangle(c.dc, x, y, x + 14 + rng.r(40), y + 10 + rng.r(34)); }
    SelectObject(c.dc, ob); SelectObject(c.dc, op); DeleteObject(b); DeleteObject(p); }
  const auto road = [&](std::vector<POINT>& pts, bool major) {
    HPEN outline = CreatePen(PS_SOLID, major ? 12 : 8, major ? RGB(222, 170, 80) : RGB(200, 196, 190)), inner = CreatePen(PS_SOLID, major ? 9 : 5, major ? RGB(252, 214, 112) : RGB(255, 255, 255));
    auto op = SelectObject(c.dc, outline); Polyline(c.dc, pts.data(), int(pts.size())); SelectObject(c.dc, inner); Polyline(c.dc, pts.data(), int(pts.size())); SelectObject(c.dc, op); DeleteObject(outline); DeleteObject(inner);
  };
  for (int gx = rng.r(120); gx < c.w; gx += 140 + rng.r(160)) { std::vector<POINT> pts; int x = gx; for (int y = -20; y < c.h + 220; y += 200) { pts.push_back(POINT{x, y}); x += rng.r(31) - 15; } road(pts, rng.r(4) == 0); }
  for (int gy = rng.r(120); gy < c.h; gy += 130 + rng.r(150)) { std::vector<POINT> pts; int y = gy; for (int x = -20; x < c.w + 220; x += 200) { pts.push_back(POINT{x, y}); y += rng.r(31) - 15; } road(pts, rng.r(4) == 0); }
  for (int k = 0; k < c.w * c.h / 30000; ++k) { const int x = rng.r(c.w), y = rng.r(c.h); text(c, fonts.get(k % 3 ? 12 : 14, 0, k % 5 == 0), RGB(68, 68, 68), x, y, Rect{0, 0, c.w, c.h}, kPlaces[k % 16]); }
}

// ------------------------------------------------------------ scenes (60 Hz ticks)
struct Scene {
  int w, h; std::string name; Rect staticRect, movingRect;
  Scene(int W, int H, std::string n) : w(W), h(H), name(std::move(n)) {}
  virtual ~Scene() = default;
  virtual int count() const = 0;
  virtual void render(int i, std::vector<uint8_t>& bgra) = 0;
  virtual bool paged() const { return false; }
  virtual int pageOf(int) const { return 0; }
};
struct ScrollScene : Scene {
  int frames, speed; Fonts fonts; std::unique_ptr<Canvas> chrome, page;
  ScrollScene(int W, int H, int n, int s) : Scene(W, H, "scroll"), frames(n), speed(s) {
    chrome = std::make_unique<Canvas>(W, 64, RGB(255, 255, 255)); drawChrome(*chrome, fonts, 1);
    page = std::make_unique<Canvas>(W, H + speed * n + 16, RGB(255, 255, 255)); drawDocument(*page, fonts, Rect{0, 0, W, page->h}, 1);
    GdiFlush(); staticRect = {0, 0, W, 64}; movingRect = {0, 64, W, H};
  }
  int count() const override { return frames; }
  void render(int i, std::vector<uint8_t>& out) override { out.resize(size_t(w) * h * 4); std::memcpy(out.data(), chrome->px, size_t(w) * 64 * 4); std::memcpy(out.data() + size_t(w) * 64 * 4, page->row(64 + i * speed), size_t(w) * (h - 64) * 4); }
};
struct MapScene : Scene {
  int frames, sw; Fonts fonts; std::unique_ptr<Canvas> side, map;
  MapScene(int W, int H, int n) : Scene(W, H, "map"), frames(n) {
    sw = (W / 4) & ~1; side = std::make_unique<Canvas>(sw, H, RGB(255, 255, 255)); drawDocument(*side, fonts, Rect{0, 0, sw, H}, 3); side->vline(sw - 1, 0, H, RGB(200, 200, 200));
    map = std::make_unique<Canvas>(W - sw + 4 * n + 8, H + 2 * n + 8, RGB(242, 239, 233)); drawMap(*map, fonts, 7);
    GdiFlush(); staticRect = {0, 0, sw, H}; movingRect = {sw, 0, W, H};
  }
  int count() const override { return frames; }
  void render(int i, std::vector<uint8_t>& out) override {
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) { uint8_t* d = out.data() + size_t(y) * w * 4; std::memcpy(d, side->row(y), size_t(sw) * 4); std::memcpy(d + size_t(sw) * 4, map->row(y + 2 * i) + size_t(4 * i) * 4, size_t(w - sw) * 4); }
  }
};
// Whole-page switches every 2 s; the production policy decides refinement/refresh.
struct PageScene : Scene {
  int pages, per = 2 * fps; Fonts fonts; std::unique_ptr<Canvas> canvas; int cached = -1;
  PageScene(int W, int H, int p) : Scene(W, H, "page"), pages(p) { staticRect = {0, 0, W, H}; }
  int count() const override { return pages * per; }
  bool paged() const override { return true; }
  int pageOf(int i) const override { return i / per; }
  void render(int i, std::vector<uint8_t>& out) override {
    const int page = i / per;
    if (page != cached) { canvas = std::make_unique<Canvas>(w, h, RGB(255, 255, 255)); drawChrome(*canvas, fonts, page + 11); drawDocument(*canvas, fonts, Rect{0, 64, w, h}, page + 11); GdiFlush(); cached = page; }
    out.assign(canvas->px, canvas->px + size_t(w) * h * 4);
  }
};
// A static document with one glyph-sized block typed every 50 ms (20 chars/s).
struct TypingScene : Scene {
  int frames; Fonts fonts; std::unique_ptr<Canvas> canvas;
  TypingScene(int W, int H, int n) : Scene(W, H, "typing"), frames(n) {
    canvas = std::make_unique<Canvas>(W, H, RGB(255, 255, 255)); drawChrome(*canvas, fonts, 5); drawDocument(*canvas, fonts, Rect{0, 64, W, H}, 5); GdiFlush();
    staticRect = {0, 0, W, H}; movingRect = {40, 300, std::min(W - 40, 40 + 9 * (n / 3 + 1)), 316};
  }
  int count() const override { return frames; }
  void render(int i, std::vector<uint8_t>& out) override {
    out.assign(canvas->px, canvas->px + size_t(w) * h * 4);
    const int typed = i / 3;
    for (int k = 0; k <= typed; ++k) {
      const int x = 40 + 9 * k; if (x + 7 > w - 40) break;
      for (int y = 300; y < 316; ++y) for (int dx = 0; dx < 7; ++dx) if ((dx + y + k) % 3) { uint8_t* p = out.data() + (size_t(y) * w + x + dx) * 4; p[0] = 30; p[1] = 30; p[2] = 30; }
    }
  }
};

// Motion-like content (video, a game, a camera pan): a smooth natural-looking
// texture with fine grain, panned at sub-pixel speed (3.37 / 1.61 px per frame,
// bilinear) under a slow fade, with a bright object crossing it. Nearly every
// block changes every frame and full-pel motion search cannot match it exactly.
uint32_t hash2(int x, int y, uint32_t seed) { uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
double valueNoise(double x, double y, uint32_t seed) {
  const int xi = int(std::floor(x)), yi = int(std::floor(y)); double fx = x - xi, fy = y - yi; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
  const auto v = [&](int dx, int dy) { return double(hash2(xi + dx, yi + dy, seed) & 0xffff) / 65535.0; };
  return (v(0, 0) * (1 - fx) + v(1, 0) * fx) * (1 - fy) + (v(0, 1) * (1 - fx) + v(1, 1) * fx) * fy;
}
struct VideoScene : Scene {
  int frames, tw, th; std::vector<uint8_t> texture;
  VideoScene(int W, int H, int n) : Scene(W, H, "video"), frames(n) {
    tw = W + 4 * n + 8; th = H + 2 * n + 8; texture.resize(size_t(tw) * th * 4);
    for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) {
      uint8_t* p = texture.data() + (size_t(y) * tw + x) * 4;
      for (int c = 0; c < 3; ++c) {
        const double v = 0.55 * valueNoise(x / 180.0, y / 180.0, 1 + c) + 0.3 * valueNoise(x / 37.0, y / 37.0, 7 + c) + 0.15 * valueNoise(x / 6.0, y / 6.0, 13 + c);
        const int grain = int(hash2(x, y, 29 + c) % 13) - 6;
        p[c] = uint8_t(std::clamp(int(20 + v * 215) + grain, 0, 255));
      }
      p[3] = 255;
    }
    movingRect = {0, 0, W, H};
  }
  int count() const override { return frames; }
  void render(int i, std::vector<uint8_t>& out) override {
    out.resize(size_t(w) * h * 4);
    const int ox = int(i * 3.37 * 256), oy = int(i * 1.61 * 256), fx = ox & 255, fy = oy & 255, bx = ox >> 8, by = oy >> 8;
    const int fade = int(256 * (0.85 + 0.15 * std::sin(i / 20.0)));
    const int cx = (i * 9) % w, cy = h / 3 + int(h / 4 * std::sin(i / 15.0)), radius = std::max(24, h / 12);
    for (int y = 0; y < h; ++y) {
      const uint8_t* r0 = texture.data() + (size_t(y + by) * tw + bx) * 4; const uint8_t* r1 = r0 + size_t(tw) * 4; uint8_t* d = out.data() + size_t(y) * w * 4;
      for (int x = 0; x < w; ++x) {
        const bool disc = (x - cx) * (x - cx) + (y - cy) * (y - cy) < radius * radius;
        for (int c = 0; c < 3; ++c) {
          const int top = r0[x * 4 + c] * (256 - fx) + r0[x * 4 + 4 + c] * fx, bottom = r1[x * 4 + c] * (256 - fx) + r1[x * 4 + 4 + c] * fx;
          const int value = ((top * (256 - fy) + bottom * fy) >> 16) * fade >> 8;
          d[x * 4 + c] = uint8_t(disc ? std::min(255, value + 120) : value);
        }
        d[x * 4 + 3] = 255;
      }
    }
  }
};

// The page scene over a photo-like background (a wallpaper, a picture behind
// the text): every near-white pixel takes a smooth grained texture, its seed
// changing with the page. Natural content keeps refinement busy far longer
// than text on white, which settles in a few frames.
struct PhotoPageScene : PageScene {
  int photoCached = -1; std::vector<uint8_t> photo;
  PhotoPageScene(int W, int H, int p) : PageScene(W, H, p) { name = "photo"; }
  void render(int i, std::vector<uint8_t>& out) override {
    PageScene::render(i, out);
    const int page = i / per;
    if (page != photoCached) {
      photo = out;
      for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint8_t* p = photo.data() + (size_t(y) * w + x) * 4;
        if (std::min({p[0], p[1], p[2]}) < 250) continue;
        for (int c = 0; c < 3; ++c) {
          const double v = 0.6 * valueNoise(x / 150.0, y / 150.0, 41 + c + page * 7) + 0.3 * valueNoise(x / 31.0, y / 31.0, 53 + c + page * 7) + 0.1 * valueNoise(x / 5.0, y / 5.0, 67 + c);
          const int grain = int(hash2(x, y, 71 + c + page) % 9) - 4;
          p[c] = uint8_t(std::clamp(int(120 + v * 125) + grain, 0, 255));
        }
      }
      photoCached = page;
    }
    out = photo;
  }
};

// ------------------------------------------------------------ cases
struct Config { std::string name; bool explicitX264 = false, sse2 = false; host::EncoderBackend backend = host::EncoderBackend::automatic; host::X264Settings settings; };
Config parseConfig(const std::string& text) {
  Config config; config.name = text;
  if (text == "mft") { config.backend = host::EncoderBackend::mft; return config; }
  if (text == "x264") { config.backend = host::EncoderBackend::x264; return config; }
  require(text.rfind("x264/", 0) == 0, "Unknown encoder " + text);
  config.explicitX264 = true; std::stringstream parts(text.substr(5)); std::string item;
  while (std::getline(parts, item, '/')) {
    const auto at = item.find('='); require(at != std::string::npos, "Bad encoder option " + item);
    const auto key = item.substr(0, at), value = item.substr(at + 1);
    if (key == "kernel") { require(value == "sse2", "kernel=sse2 only"); config.sse2 = true; }  // the conversion without AVX2
    else if (key == "preset") config.settings.preset = value; else if (key == "profile") config.settings.profile = value;
    else if (key == "crf") config.settings.crf = std::stod(value); else if (key == "buffer") config.settings.bufferMs = std::stoi(value);
    else if (key == "threads") config.settings.threads = std::stoi(value); else if (key == "maxbuffer") config.settings.maximumBufferKbit = std::stoi(value);
    else if (key == "cpu") config.settings.cpu = value;
    else if (key == "constant") config.settings.constantBlocks = value != "0";
    else if (key.rfind("x.", 0) == 0) config.settings.parameters.push_back({key.substr(2), value});  // x264_param_parse name=value
    else throw std::runtime_error("Unknown encoder option " + key);
  }
  return config;
}
struct Encoded { std::vector<uint8_t> bytes; uint64_t pts = 0; bool key = false; int index = 0; };
struct Row { bool key = false, submitted = false, refine = false, changed = false, decoded = false; size_t bytes = 0; double encodeMs = 0, cpuMs = 0, convertMs = 0, convertCpuMs = 0, policyMs = 0, inkMoving = 0, inkStatic = 0, all = 0; int bitrate = 0; };
struct Accumulator { double sq = 0, inkSq = 0; uint64_t n = 0, inkN = 0; };
void accumulate(Rect r, int W, const uint8_t* source, const uint8_t* reference, const uint8_t* decoded, Accumulator& a) {
  for (int y = r.y0; y < r.y1; ++y) for (int x = r.x0; x < r.x1; ++x) {
    const size_t at = (size_t(y) * W + x) * 4; const bool ink = std::min({source[at], source[at + 1], source[at + 2]}) < 200;
    for (int c = 0; c < 3; ++c) { const double e = double(reference[at + c]) - double(decoded[at + c]); a.sq += e * e; ++a.n; if (ink) { a.inkSq += e * e; ++a.inkN; } }
  }
}
std::ofstream results;
double cyclesMs = 1;
bool pace = true;
bool tilesOn = false;  // tiles: lossless tile source + helper in the tick, as agent.hpp does after the client opts in
// share: the tiles get what agent.hpp gives an auto session, the part of the
// target video left unused over the last 0.5 s (at least 62,500 B/s), not the
// whole target.
bool tileShareOn = false;
// tilefloor=N: the share never below N bytes per second (agent.hpp's minimum).
size_t tileShareFloor = 62500;
// refinecap=N (tiles only): refinement episodes end after N frames.
uint32_t refineCap = 0;
bool shareSkipRefinement = false;
host::FrameRefinement refinementFor(int bitrate) { auto value = host::FrameRefinement::forCbr(bitrate, fps); if (tilesOn && refineCap) value.maximumFrames = refineCap; return value; }
int idrEvery = 0;  // idr=N: ask for an IDR every N frames (the send queue's requestKeyFrame after dropped video)

void runCase(Scene& scene, const Config& config, const std::string& rateName) {
  const int W = scene.w, H = scene.h, N = scene.count();
  const bool automatic = rateName == "auto";
  struct Kernel { explicit Kernel(bool sse2) { host::pixels::forceSse2Kernel = sse2; } ~Kernel() { host::pixels::forceSse2Kernel = false; } } kernel(config.sse2);
  const int floor = std::min(host::adaptiveBitrateFloor(W, H, fps), host::adaptiveMaximumBitrate);
  // auto: the controller's start target, then scripted live retargets every 0.5 s
  // (agent.hpp: clamp to the floor, setBitrate, policy.retarget on success).
  const std::vector<int> schedule{20000000, 12000000, 8000000, 16000000, 30000000, 40000000, 24000000, 10000000};
  int applied = automatic ? std::clamp(schedule[0], floor, host::adaptiveMaximumBitrate) : std::stoi(rateName) * 1000000;
  std::unique_ptr<host::Encoder> encoder = config.explicitX264 ? std::make_unique<host::Encoder>(W, H, fps, applied, config.settings)
                                                              : std::make_unique<host::Encoder>(W, H, fps, applied, host::EncoderTuning{}, config.backend);
  host::FrameEncodePolicy policy(size_t(W) * H * 3 / 2, refinementFor(applied));
  std::unique_ptr<host::LosslessTileSource> tiles; std::unique_ptr<host::ParallelWork> helper; std::vector<double> tickMs, tileMs;
  std::atomic<size_t> share{size_t(applied) / 8}; std::deque<std::pair<uint64_t, size_t>> recentVideo; size_t recentVideoBytes = 0;
  if (tilesOn) {
    auto settings = host::LosslessSettings::forBitrate(applied);
    if (tileShareOn) { settings = host::LosslessSettings::forTargetRate([&share] { return share.load(); }); settings.minimumRateBytesPerSecond = tileShareFloor; }
    tiles = std::make_unique<host::LosslessTileSource>(uint32_t(W), uint32_t(H), settings); helper = std::make_unique<host::ParallelWork>();
  }
  // Tiles the client shows losslessly after each tick (tiles only).
  std::vector<uint32_t> presentAfter(static_cast<size_t>(N), 0);
  std::vector<uint8_t> bgra, nv12(size_t(W) * H * 3 / 2); std::vector<Encoded> stream; std::vector<Row> rows(static_cast<size_t>(N));
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); require(timer != nullptr, "timer");
  const auto start = Clock::now(); const ULONG64 cycles0 = processCycles(); ULONG64 outside = 0; size_t schedulePosition = 0;
  for (int i = 0; i < N; ++i) {
    ULONG64 a = 0, b = 0, ca = 0; QueryThreadCycleTime(GetCurrentThread(), &a);
    scene.render(i, bgra);
    // The capture path's conversion (agent.hpp: Capture::frame), timed apart from the encoder.
    QueryThreadCycleTime(GetCurrentThread(), &ca); const auto converting = Clock::now();
    host::bgraToNv12(bgra.data(), nv12.data(), W, H);
    const double convertMs = millis(Clock::now() - converting);
    QueryThreadCycleTime(GetCurrentThread(), &b); outside += b - a;
    rows[size_t(i)].convertMs = convertMs; rows[size_t(i)].convertCpuMs = double(b - ca) / cyclesMs;
    if (pace) { const auto deadline = start + std::chrono::microseconds(int64_t(tickUs(i))); const auto now = Clock::now();
      if (deadline > now) { LARGE_INTEGER due; due.QuadPart = -int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() / 100); SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, INFINITE); } }
    if (automatic && i && i % 30 == 0) {
      const int wanted = std::clamp(schedule[++schedulePosition % schedule.size()], floor, host::adaptiveMaximumBitrate);
      if (wanted != applied && encoder->setBitrate(wanted)) { applied = wanted; policy.retarget(refinementFor(wanted)); }
    }
    auto& row = rows[size_t(i)]; row.bitrate = applied;
    const bool forceKey = idrEvery && i && i % idrEvery == 0; if (forceKey) encoder->requestKeyFrame();
    host::FrameEncodeDecision decision; std::vector<host::Frame> outputs; ULONG64 c0 = 0, c1 = 0; ULONG64 p0 = 0; Clock::time_point t0, t1;
    if (tiles) {
      // agent.hpp: every new WGC frame reports "dirty anywhere", so the whole composed frame is compared.
      host::BufferComposedFrame composed(bgra.data(), W, H, std::nullopt);
      QueryThreadCycleTime(GetCurrentThread(), &c0); p0 = processCycles(); t0 = Clock::now();
      const auto tick = host::losslessTick(*tiles, *helper, composed, policy, nv12, tickUs(i), [&] { encoder->encode(nv12.data(), tickUs(i), [&](host::Frame f) { outputs.push_back(std::move(f)); }); }, forceKey);
      t1 = Clock::now(); decision = tick.decision; tickMs.push_back(millis(t1 - t0)); tileMs.push_back(tick.tileMs);
      presentAfter[size_t(i)] = tiles->presentTiles();
      if (!decision.submit) continue;
    } else {
      const auto preparing = Clock::now(); decision = policy.prepare(nv12, tickUs(i)); row.policyMs = millis(Clock::now() - preparing); if (!decision.submit) continue;
      QueryThreadCycleTime(GetCurrentThread(), &c0); p0 = processCycles(); t0 = Clock::now();
      encoder->encode(nv12.data(), tickUs(i), [&](host::Frame f) { outputs.push_back(std::move(f)); });
      t1 = Clock::now();
    }
    row.submitted = true; row.refine = decision.refine; row.changed = decision.contentChanged; const ULONG64 p1 = processCycles(); QueryThreadCycleTime(GetCurrentThread(), &c1);
    row.encodeMs = millis(t1 - t0); row.cpuMs = double(p1 - p0) / cyclesMs;
    for (auto& f : outputs) {
      row.key = f.key;
      require(policy.output(f.ptsUs, f.key, tickUs(i), f.bytes.size()), "Policy rejected an encoder output");
      const int index = int((f.ptsUs * fps + 500000) / 1000000); require(index >= 0 && index < N && tickUs(index) == f.ptsUs, "Unmatched output pts");
      rows[size_t(index)].bytes = f.bytes.size();
      // agent.hpp shareTiles: video bytes over the last 0.5 s, doubled, off the target.
      // shareskip: refinement outputs do not reduce the tiles' share.
      const size_t counted = shareSkipRefinement && rows[size_t(index)].refine ? 0 : f.bytes.size();
      recentVideo.push_back({tickUs(i), counted}); recentVideoBytes += counted;
      while (tickUs(i) - recentVideo.front().first > 500000) { recentVideoBytes -= recentVideo.front().second; recentVideo.pop_front(); }
      { const size_t used = recentVideoBytes * 2, target = size_t(applied) / 8; share = target > used ? target - used : 0; }
      stream.push_back({std::move(f.bytes), f.ptsUs, f.key, index});
    }
  }
  const double loopMs = millis(Clock::now() - start); const double encoderCpuMs = double(processCycles() - cycles0 - outside) / cyclesMs;
  CloseHandle(timer);
  encoder->finish([&](host::Frame f) { const int index = int((f.ptsUs * fps + 500000) / 1000000); rows[size_t(index)].bytes = f.bytes.size(); stream.push_back({std::move(f.bytes), f.ptsUs, f.key, index}); });
  // Decode pass (the client's RemoteDecoder), scored against re-rendered sources.
  multitap::RemoteDecoder decoder; host::check(decoder.Initialize(UINT(W), UINT(H)), "Decoder");
  std::vector<uint8_t> source, sourceNv12(size_t(W) * H * 3 / 2), reference(size_t(W) * H * 4); size_t decoded = 0, keys = 0, maxBytes = 0, total = 0;
  std::vector<double> decodeMs;
  const auto score = [&](const multitap::DecodedFrame& f) {
    const int index = int((f.pts_us * fps + 500000) / 1000000); if (index < 0 || index >= N) return;
    scene.render(index, source); host::bgraToNv12(source.data(), sourceNv12.data(), W, H);
    multitap::video::nv12ToRgba(sourceNv12.data(), reference.data(), UINT(W), UINT(H), UINT(W), UINT(H), false, false);
    auto& row = rows[size_t(index)]; row.decoded = true;
    Accumulator all; accumulate(Rect{0, 0, W, H}, W, source.data(), reference.data(), f.rgba.data(), all); row.all = psnr(all.sq / double(std::max<uint64_t>(all.n, 1)));
    if (!scene.movingRect.empty()) { Accumulator m; accumulate(scene.movingRect, W, source.data(), reference.data(), f.rgba.data(), m); row.inkMoving = psnr(m.inkSq / double(std::max<uint64_t>(m.inkN, 1))); }
    if (!scene.staticRect.empty()) { Accumulator s; accumulate(scene.staticRect, W, source.data(), reference.data(), f.rgba.data(), s); row.inkStatic = psnr(s.inkSq / double(std::max<uint64_t>(s.inkN, 1))); }
  };
  for (const auto& e : stream) {
    keys += e.key; maxBytes = std::max(maxBytes, e.bytes.size()); total += e.bytes.size();
    const auto t0 = Clock::now(); double scoring = 0;
    host::check(decoder.Decode(e.bytes.data(), e.bytes.size(), e.pts, [&](multitap::DecodedFrame&& f) { ++decoded; const auto s0 = Clock::now(); score(f); scoring += millis(Clock::now() - s0); }), "Client decode");
    decodeMs.push_back(millis(Clock::now() - t0) - scoring);
  }
  host::check(decoder.Drain([&](multitap::DecodedFrame&& f) { ++decoded; score(f); }), "Drain");
  // Link model: frames leave in order over a link draining at the target rate.
  std::vector<double> delivered(static_cast<size_t>(N), 0.0); double busy = 0;
  for (int i = 0; i < N; ++i) if (rows[size_t(i)].bytes) { busy = std::max(busy, double(tickUs(i)) / 1000 + rows[size_t(i)].encodeMs) + double(rows[size_t(i)].bytes) * 8000.0 / rows[size_t(i)].bitrate; delivered[size_t(i)] = busy; }
  std::vector<double> encodeMs, idrMs, cpuMs, moving, still, linkDelay; size_t submitted = 0, refinements = 0;
  std::vector<double> convertMs, policyMs, pipelineMs, pipelineCpuMs;
  for (int i = 0; i < N; ++i) { const auto& r = rows[size_t(i)]; if (!r.submitted) continue; ++submitted; refinements += r.refine; encodeMs.push_back(r.encodeMs); if (r.key) idrMs.push_back(r.encodeMs); cpuMs.push_back(r.cpuMs); if (r.bytes) linkDelay.push_back(delivered[size_t(i)] - double(tickUs(i)) / 1000);
    convertMs.push_back(r.convertMs); policyMs.push_back(r.policyMs); pipelineMs.push_back(r.convertMs + r.policyMs + r.encodeMs); pipelineCpuMs.push_back(r.convertCpuMs + r.policyMs + r.cpuMs); }
  for (int i = N / 2; i < N; ++i) { const auto& r = rows[size_t(i)]; if (r.decoded) { moving.push_back(r.inkMoving); still.push_back(r.inkStatic); } }
  std::ostringstream j; j << std::fixed << std::setprecision(2);
  j << "{\"scene\":\"" << scene.name << "\",\"width\":" << W << ",\"height\":" << H << ",\"rate\":\"" << rateName << "\",\"encoder\":\"" << config.name << "\",\"backend\":\"" << encoder->backendName() << "\"";
  if (encoder->backend() == host::EncoderBackend::x264) {
    const auto settings = config.explicitX264 ? config.settings : host::X264Settings::production(); const auto rate = host::x264RateControl(W, H, fps, automatic ? schedule[0] : applied, settings);
    j << ",\"x264\":{\"preset\":\"" << settings.preset << "\",\"profile\":\"" << settings.profile << "\",\"crf\":" << rate.crf << ",\"vbvMaxKbps\":" << rate.maximumKbps << ",\"vbvBufferKbit\":" << rate.bufferKbit << ",\"threads\":" << settings.threads << ",\"cpu\":\"" << (settings.cpu.empty() ? "detected" : settings.cpu) << "\",\"constantBlocks\":" << (settings.constantBlocks ? "true" : "false") << ",\"parameters\":\"";
    for (const auto& [name, value] : settings.parameters) j << name << '=' << value << ';';
    j << "\"}";
  }
  j << ",\"conversion\":\"" << (host::pixels::rowPairKernel() == host::pixels::bgraToNv12RowPairAvx2 ? "avx2" : "sse2") << '"';
  j << ",\"frames\":" << N << ",\"submitted\":" << submitted << ",\"refinements\":" << refinements << ",\"decoded\":" << decoded << ",\"keyFrames\":" << keys
    << ",\"mbps\":" << double(total) * 8 / (double(N) / fps) / 1e6 << ",\"maxFrameBytes\":" << maxBytes
    << ",\"encodeP50Ms\":" << percentile(encodeMs, .5) << ",\"encodeP95Ms\":" << percentile(encodeMs, .95) << ",\"encodeP90Ms\":" << percentile(encodeMs, .9) << ",\"encodeP99Ms\":" << percentile(encodeMs, .99) << ",\"encodeMaxMs\":" << percentile(encodeMs, 1) << ",\"firstFrameMs\":" << rows[0].encodeMs << ",\"steadyMaxMs\":" << percentile(std::vector<double>(encodeMs.begin() + (encodeMs.size() > 1 ? 1 : 0), encodeMs.end()), 1) << ",\"tiles\":" << (tilesOn ? "true" : "false") << ",\"tickP50Ms\":" << percentile(tickMs, .5) << ",\"tickP99Ms\":" << percentile(tickMs, .99) << ",\"tickMaxMs\":" << percentile(tickMs, 1) << ",\"tileP50Ms\":" << percentile(tileMs, .5) << ",\"tileMaxMs\":" << percentile(tileMs, 1) << ",\"idrCount\":" << idrMs.size() << ",\"idrP50Ms\":" << percentile(idrMs, .5) << ",\"idrMaxMs\":" << percentile(idrMs, 1)
    << ",\"cpuPerSubmitMs\":" << mean(cpuMs) << ",\"cpuP95Ms\":" << percentile(cpuMs, .95) << ",\"encoderCores\":" << encoderCpuMs / std::max(1e-9, loopMs)
    // One capture thread's serial work per submitted frame (conversion + policy + encode call),
    // without the capture readback; pipelineFps is what it sustains when every frame changes.
    << ",\"convertP50Ms\":" << percentile(convertMs, .5) << ",\"policyP50Ms\":" << percentile(policyMs, .5)
    << ",\"pipelineMeanMs\":" << mean(pipelineMs) << ",\"pipelineFps\":" << 1000.0 / std::max(1e-9, mean(pipelineMs)) << ",\"encodeFps\":" << 1000.0 / std::max(1e-9, mean(encodeMs))
    << ",\"pipelineCpuMs\":" << mean(pipelineCpuMs)
    << ",\"decodeP95Ms\":" << percentile(decodeMs, .95) << ",\"linkDelayP50Ms\":" << percentile(linkDelay, .5) << ",\"linkDelayP95Ms\":" << percentile(linkDelay, .95);
  if (!scene.paged()) j << ",\"steadyMovingInkDb\":" << mean(moving) << ",\"steadyStaticInkDb\":" << mean(still);
  else {
    // Per page change (page 0 is the stream start): first decoded frame's ink,
    // time to 35 dB ink (pts and link-delivered), ink at the end of the page.
    std::vector<double> first, firstBytes, firstLink, tts, ttsLink, last; int missed = 0;
    for (int p = 1; p < scene.count() / static_cast<PageScene&>(scene).per; ++p) {
      const int begin = p * static_cast<PageScene&>(scene).per, end = begin + static_cast<PageScene&>(scene).per; bool reached = false, firstSet = false; double lastInk = 0;
      for (int i = begin; i < end; ++i) { const auto& r = rows[size_t(i)]; if (!r.decoded) continue;
        // The first changed frame: its ink, size and when the link delivered it (first visible change).
        if (!firstSet) { first.push_back(r.inkStatic); firstBytes.push_back(double(r.bytes)); firstLink.push_back(delivered[size_t(i)] - double(tickUs(begin)) / 1000); firstSet = true; }
        if (!reached && r.inkStatic >= 35) { tts.push_back(double(tickUs(i) - tickUs(begin)) / 1000); ttsLink.push_back(delivered[size_t(i)] - double(tickUs(begin)) / 1000); reached = true; }
        lastInk = r.inkStatic; }
      if (!reached) ++missed; last.push_back(lastInk);
    }
    j << ",\"switchFirstInkDb\":" << mean(first) << ",\"switchFirstBytes\":" << mean(firstBytes) << ",\"switchFirstLinkMs\":" << mean(firstLink) << ",\"tts35MeanMs\":" << (tts.empty() ? -1 : mean(tts)) << ",\"tts35LinkMaxMs\":" << (ttsLink.empty() ? -1 : percentile(ttsLink, 1)) << ",\"tts35Missed\":" << missed << ",\"pageEndInkDb\":" << mean(last)
      << ",\"startIdrInkDb\":" << rows[0].inkStatic;
    if (tiles) {
      // Per page change: from the change to the first tick after which every
      // tile is shown losslessly (the picture is then exact).
      std::vector<double> full; int fullMissed = 0; const int per = static_cast<PageScene&>(scene).per;
      for (int p = 0; p < scene.count() / per; ++p) {
        const int begin = p * per; bool reached = false;
        for (int i = begin; i < begin + per; ++i) if (presentAfter[size_t(i)] == tiles->tileCount()) { full.push_back(double(tickUs(i) - tickUs(begin)) / 1000); reached = true; break; }
        if (!reached) ++fullMissed;
      }
      j << ",\"tileShare\":" << (tileShareOn ? "true" : "false") << ",\"losslessFullMeanMs\":" << (full.empty() ? -1 : mean(full)) << ",\"losslessFullMaxMs\":" << (full.empty() ? -1 : percentile(full, 1)) << ",\"losslessFullMissed\":" << fullMissed;
    }
  }
  j << ",\"fallback\":" << encoder->fallbackCode() << ",\"synthetic\":true,\"desktopCaptured\":false,\"relayUsed\":false}";
  require(decoded == stream.size(), "Not every encoded frame decoded (" + config.name + ")");
  std::cout << j.str() << std::endl; results << j.str() << "\n"; results.flush();
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc >= 2, "Usage: MultitapHostEncoderBenchmark NEW_OUTPUT_DIR [options]");
    const fs::path root(argv[1]); require(!fs::exists(root), "Use a new output directory");
    int cores = 0, frames = 180; bool idle = false; std::vector<std::string> heights{"760", "1080", "1440"}; std::vector<std::string> rates{"8", "20", "40", "auto"}, scenes{"scroll", "map", "page", "typing"}, encoders;
    const auto list = [](const std::string& s) { std::vector<std::string> r; std::stringstream ss(s); std::string item; while (std::getline(ss, item, ',')) r.push_back(item); return r; };
    for (int a = 2; a < argc; ++a) {
      std::string n; for (const wchar_t* c = argv[a]; *c; ++c) n.push_back(char(*c));
      if (n.rfind("cores=", 0) == 0) cores = std::stoi(n.substr(6));
      else if (n.rfind("frames=", 0) == 0) frames = std::stoi(n.substr(7));
      else if (n.rfind("sizes=", 0) == 0) heights = list(n.substr(6));
      else if (n == "idle") idle = true;  // run at IDLE priority (a shared desktop machine), not ABOVE_NORMAL
      else if (n.rfind("rates=", 0) == 0) rates = list(n.substr(6));
      else if (n.rfind("scenes=", 0) == 0) scenes = list(n.substr(7));
      else if (n.rfind("enc=", 0) == 0) encoders.push_back(n.substr(4));
      else if (n.rfind("idr=", 0) == 0) idrEvery = std::stoi(n.substr(4));
      else if (n == "tiles") tilesOn = true;
      else if (n == "share") tileShareOn = true;
      else if (n == "shareskip") tileShareOn = shareSkipRefinement = true;
      else if (n.rfind("refinecap=", 0) == 0) refineCap = uint32_t(std::stoul(n.substr(10)));
      else if (n.rfind("tilefloor=", 0) == 0) tileShareFloor = size_t(std::stoull(n.substr(10)));
      else if (n == "nopace") pace = false;
      else throw std::runtime_error("Unknown option " + n);
    }
    if (encoders.empty()) encoders = {"mft", "x264"};
    require(frames >= 30 && frames <= 600, "frames must be 30-600");
    fs::create_directories(root);
    if (cores > 0) require(cores < 64 && SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR(1) << cores) - 1), "SetProcessAffinityMask");
    host::Runtime runtime; SetPriorityClass(GetCurrentProcess(), idle ? IDLE_PRIORITY_CLASS : ABOVE_NORMAL_PRIORITY_CLASS);
    cyclesMs = cyclesPerMs();
    results.open(root / "results.jsonl");
    { SYSTEM_INFO si{}; GetSystemInfo(&si); DWORD_PTR pm = 0, sm = 0; GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm); int active = 0; for (DWORD_PTR m = pm; m; m >>= 1) active += int(m & 1);
      std::ofstream info(root / "run-info.json"); info << "{\"logicalProcessors\":" << si.dwNumberOfProcessors << ",\"affinityProcessors\":" << active << ",\"tscCyclesPerMs\":" << std::fixed << std::setprecision(0) << cyclesMs << ",\"frames\":" << frames << ",\"paced\":" << (pace ? "true" : "false") << ",\"idle\":" << (idle ? "true" : "false") << ",\"x264Built\":" << (host::x264EncoderAvailable() ? "true" : "false") << "}\n"; }
    std::vector<Config> configs; for (const auto& e : encoders) configs.push_back(parseConfig(e));
    const std::map<int, std::pair<int, int>> sizes{{760, {1264, 760}}, {1080, {1920, 1080}}, {1440, {2560, 1440}}, {2160, {3840, 2160}}};
    for (const auto& size : heights) {
      // A known height (760, 1080, 1440, 2160) or WIDTHxHEIGHT (even, e.g. 5120x1440).
      int W = 0, H = 0; const auto cross = size.find('x');
      if (cross != std::string::npos) { W = std::stoi(size.substr(0, cross)); H = std::stoi(size.substr(cross + 1)); require(W >= 64 && H >= 64 && W <= 8192 && H <= 4320 && !((W | H) & 1), "Bad size " + size); }
      else { const auto found = sizes.find(std::stoi(size)); require(found != sizes.end(), "Unknown size " + size); W = found->second.first; H = found->second.second; }
      for (const auto& name : scenes) {
        std::unique_ptr<Scene> scene;
        if (name == "scroll") scene = std::make_unique<ScrollScene>(W, H, frames, 6);
        else if (name == "map") scene = std::make_unique<MapScene>(W, H, frames);
        else if (name == "page") scene = std::make_unique<PageScene>(W, H, 4);
        else if (name == "photo") scene = std::make_unique<PhotoPageScene>(W, H, 4);
        else if (name == "typing") scene = std::make_unique<TypingScene>(W, H, frames);
        else if (name == "video") scene = std::make_unique<VideoScene>(W, H, frames);
        else throw std::runtime_error("Unknown scene " + name);
        for (const auto& rate : rates) for (const auto& config : configs) runCase(*scene, config, rate);
      }
    }
    return 0;
  } catch (const std::exception& e) { std::cerr << "fatal: " << e.what() << "\n"; return 1; }
}
