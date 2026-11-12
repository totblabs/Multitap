// Synthetic host capture->encode pipeline benchmark.
// Frames are generated in memory and uploaded into an owned D3D11 texture; the
// readback stage mirrors WGC's CopyResource->staging->Map path on the same
// device type the host selects (hardware, else WARP). No desktop pixels are
// read, saved or transmitted, and no display, driver or service is changed.
#include "capture_selection.hpp"
#include "capture_compose.hpp"
#include "capture_dirty.hpp"
#include "frame_encode_policy.hpp"
#if __has_include("cursor_patch.hpp")
#include "cursor_patch.hpp"
#define MULTITAP_DIRECT_CAPTURE 1
#endif
#if __has_include("capture_scale.hpp")
#include "capture_scale.hpp"
#define MULTITAP_AREA_SCALE 1
#endif
#include <d3d11.h>
#include <cmath>
#include <tuple>
#include <type_traits>
#include <intrin.h>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>

namespace {
using host::Clock;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
double millis(Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); }
double cpuMs() {
  FILETIME c{}, e{}, k{}, u{}; require(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) != FALSE, "Process times");
  ULARGE_INTEGER kernel{}, user{}; kernel.LowPart = k.dwLowDateTime; kernel.HighPart = k.dwHighDateTime; user.LowPart = u.dwLowDateTime; user.HighPart = u.dwHighDateTime;
  return double(kernel.QuadPart + user.QuadPart) / 10000;
}
// Process-wide CPU cycles (all threads, including MFT/WARP workers), converted
// with a measured TSC rate. GetProcessTimes is too coarse per stage.
uint64_t cycles() { ULONG64 value = 0; require(QueryProcessCycleTime(GetCurrentProcess(), &value) != FALSE, "Process cycles"); return value; }
double cyclesPerMs() {
  static const double rate = [] {
    // Cycle times are TSC-based; calibrate the TSC against QPC wall time.
    LARGE_INTEGER f{}, a{}, b{}; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a); const auto c0 = __rdtsc(); Sleep(250); const auto c1 = __rdtsc(); QueryPerformanceCounter(&b);
    return double(c1 - c0) / (double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
  }();
  return rate;
}
double cpuOf(uint64_t from, uint64_t to) { return double(to - from) / cyclesPerMs(); }
struct Series {
  std::vector<double> values;
  void add(double value) { values.push_back(value); }
  double mean() const { return values.empty() ? 0 : std::accumulate(values.begin(), values.end(), 0.0) / double(values.size()); }
  double quantile(double q) const { if (values.empty()) return 0; auto sorted = values; std::sort(sorted.begin(), sorted.end()); return sorted[std::min(sorted.size() - 1, size_t(q * double(sorted.size() - 1) + 0.5))]; }
  void json(std::ostream& out) const { out << "{\"mean\":" << mean() << ",\"p50\":" << quantile(.5) << ",\"p95\":" << quantile(.95) << '}'; }
};
uint64_t digest(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 0x9e3779b97f4a7c15ull ^ bytes.size(); size_t at = 0;
  for (; at + 8 <= bytes.size(); at += 8) { uint64_t word; std::memcpy(&word, bytes.data() + at, 8); hash = (hash ^ word) * 0x100000001b3ull; hash ^= hash >> 29; }
  for (; at < bytes.size(); ++at) hash = (hash ^ bytes[at]) * 0x100000001b3ull;
  return hash;
}

enum class Scene { motion, partial, cursor, still };
bool forceX264 = false;  // --x264
const char* sceneName(Scene scene) { return scene == Scene::motion ? "motion" : scene == Scene::partial ? "partial" : scene == Scene::cursor ? "cursor-only" : "static"; }

// Deterministic desktop-like content: text rows, UI bars and a moving box.
class SceneSource {
 public:
  SceneSource(int width, int height) : width_(width), height_(height), document_(size_t(width) * height * 2 * 4) {
    uint32_t seed = 12345; auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (size_t i = 0; i < document_.size(); i += 4) { document_[i] = 250; document_[i + 1] = 248; document_[i + 2] = 245; document_[i + 3] = 255; }
    for (int line = 0; line * 18 + 14 < height * 2; ++line) {
      int x = 24 + int(next() % 40);
      while (x + 9 < width - 24) {
        const int glyph = 5 + int(next() % 4), ink = int(next() % 3);
        for (int y = line * 18 + 3; y < line * 18 + 14; ++y) for (int dx = 0; dx < glyph; ++dx) if ((next() & 3) != 0) {
          auto* p = document_.data() + (size_t(y) * width + x + dx) * 4; p[0] = uint8_t(ink == 2 ? 180 : 30); p[1] = uint8_t(ink == 1 ? 90 : 30); p[2] = uint8_t(ink == 1 ? 30 : 40);
        }
        x += glyph + 2 + ((next() % 9) == 0 ? 7 : 0);
      }
    }
  }
  // Returns whether WGC would have delivered a fresh frame. `lastDirty` then
  // holds what WGC would report as changed (the scroll: everything; the
  // partial scene: the box).
  bool render(Scene scene, int n, std::vector<uint8_t>& out) const {
    if ((scene == Scene::cursor || scene == Scene::still) && n > 0) return false;
    const size_t stride = size_t(width_) * 4; out.resize(stride * height_);
    const int scroll = scene == Scene::motion ? (n * 3) % height_ : 0;
    std::memcpy(out.data(), document_.data() + size_t(scroll) * stride, stride * height_);
    for (int y = 0; y < 40; ++y) for (int x = 0; x < width_; ++x) { auto* p = out.data() + (size_t(y) * width_ + x) * 4; p[0] = 60; p[1] = 50; p[2] = 40; }
    lastDirty.clear();
    if (scene == Scene::motion || scene == Scene::partial) {
      const int box = std::min(160, height_ / 4), bx = scene == Scene::motion ? (n * 9) % (width_ - box) : width_ / 2, by = height_ / 2;
      for (int y = by; y < by + box; ++y) for (int x = bx; x < bx + box; ++x) {
        auto* p = out.data() + (size_t(y) * width_ + x) * 4; p[0] = uint8_t(x * 2 + n * 5); p[1] = uint8_t(y + n * 3); p[2] = uint8_t(200 - n); p[3] = 255;
      }
      lastDirty.push_back(scene == Scene::motion ? host::capture::PixelRect{0, 0, width_, height_} : host::capture::PixelRect{bx, by, bx + box, by + box});
    } else lastDirty.push_back({0, 0, width_, height_});
    return true;
  }
  mutable std::vector<host::capture::PixelRect> lastDirty;
  POINT cursor(Scene scene, int n) const {
    if (scene == Scene::still) return {300, 200};
    return {LONG(100 + (n * 7) % (width_ - 200)), LONG(100 + (n * 5) % (height_ - 200))};
  }
 private:
  int width_, height_; std::vector<uint8_t> document_;
};

// Same device selection as Capture::Modern: hardware first, then WARP.
struct Gpu {
  host::Com<ID3D11Device> device; host::Com<ID3D11DeviceContext> context; host::Com<ID3D11Texture2D> frame, staging; host::Com<ID3D11Query> query; bool warp = false; int width, height;
  Gpu(int w, int h, bool forceWarp) : width(w), height(h) {
    HRESULT hr = forceWarp ? E_FAIL : D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) { warp = true; host::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Benchmark WARP device"); }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = UINT(w); desc.Height = UINT(h); desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    host::check(device->CreateTexture2D(&desc, nullptr, &frame), "Benchmark frame texture");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    host::check(device->CreateTexture2D(&desc, nullptr, &staging), "Benchmark staging texture");
    D3D11_QUERY_DESC q{}; q.Query = D3D11_QUERY_EVENT; host::check(device->CreateQuery(&q, &query), "Benchmark event query");
  }
  // Untimed: plays the compositor's role and waits until the frame exists.
  void upload(const std::vector<uint8_t>& bgra) {
    context->UpdateSubresource(frame.Get(), 0, nullptr, bgra.data(), UINT(width) * 4, 0); context->End(query.Get()); context->Flush();
    while (context->GetData(query.Get(), nullptr, 0, 0) == S_FALSE) SwitchToThread();
  }
};
class Dib {
 public:
  Dib(int width, int height) {
    dc = CreateCompatibleDC(nullptr); BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(dc && bitmap && pixels, "Benchmark DIB"); previous = SelectObject(dc, bitmap); SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
  }
  ~Dib() { if (previous) SelectObject(dc, previous); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
  HDC dc = nullptr; uint8_t* pixels = nullptr;
 private: HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr;
};
struct StageTimes { double readback = 0, compose = 0, cursor = 0, convert = 0; };

// Exact replica of the pre-change Capture::Modern::render + Capture::frame
// sequence for one unscaled owned surface (row copy, DIB copy, cursor, convert).
class LegacyCapture {
 public:
  LegacyCapture(int width, int height) : width_(width), height_(height), dib_(width, height), surface_(size_t(width) * height * 4), nv12_(size_t(width) * height * 3 / 2) {}
  const std::vector<uint8_t>& frame(Gpu& gpu, bool updated, POINT position, HCURSOR shape, StageTimes& times) {
    const auto began = Clock::now();
    if (updated) {
      gpu.context->CopyResource(gpu.staging.Get(), gpu.frame.Get()); D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(gpu.context->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark readback");
      for (int y = 0; y < height_; ++y) std::memcpy(surface_.data() + size_t(y) * width_ * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(width_) * 4);
      gpu.context->Unmap(gpu.staging.Get(), 0);
    }
    const auto readback = Clock::now();
    GdiFlush(); host::capture::copyUnscaledSurface({dib_.pixels, size_t(width_) * height_ * 4}, width_, height_, surface_, width_, height_, 0, 0);
    const auto composed = Clock::now();
    // Pay for the same cursor query, but compose the synthetic shape/position.
    CURSORINFO info{sizeof(CURSORINFO)}; (void)GetCursorInfo(&info); const bool cursorKnown = true;
    std::array<uint64_t, 6> stamp{};
    ICONINFO icon{}; require(GetIconInfo(shape, &icon) != FALSE, "Benchmark cursor");
    const int x = position.x - int(icon.xHotspot), y = position.y - int(icon.yHotspot), cw = std::max(1, GetSystemMetrics(SM_CXCURSOR)), ch = std::max(1, GetSystemMetrics(SM_CYCURSOR));
    stamp = {uint64_t(reinterpret_cast<uintptr_t>(shape)), uint64_t(uint32_t(x)), uint64_t(uint32_t(y)), uint64_t(cw), uint64_t(ch), 1};
    require(DrawIconEx(dib_.dc, x, y, shape, cw, ch, 0, nullptr, DI_NORMAL) != FALSE, "Benchmark cursor draw");
    if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor);
    GdiFlush();
    const auto cursor = Clock::now();
    const bool eligible = !updated && cursorKnown && stampValid_ && stamp == stamp_;
    if (!cache_.reuse(dib_.pixels, size_t(width_) * height_ * 4, eligible)) host::bgraToNv12(dib_.pixels, nv12_.data(), width_, height_);
    stamp_ = stamp; stampValid_ = cursorKnown;
    const auto converted = Clock::now();
    times = {millis(readback - began), millis(composed - readback), millis(cursor - composed), millis(converted - cursor)};
    return nv12_;
  }
 private:
  int width_, height_; Dib dib_; std::vector<uint8_t> surface_, nv12_; host::PixelConversionCache cache_; std::array<uint64_t, 6> stamp_{}; bool stampValid_ = false;
};

#ifdef MULTITAP_DIRECT_CAPTURE
// Mirrors the new Capture direct path (Modern::render direct branch,
// visitBase and Capture::frame cursor handling) with the same product helpers.
class DirectCapture {
 public:
  DirectCapture(int width, int height) : width_(width), height_(height), nv12_(size_t(width) * height * 3 / 2) {
    screen_ = GetDC(nullptr); require(patch_.reserve(std::max(64, GetSystemMetrics(SM_CXCURSOR) + 2), std::max(64, GetSystemMetrics(SM_CYCURSOR) + 2), screen_), "Benchmark cursor canvas");
  }
  ~DirectCapture() { ReleaseDC(nullptr, screen_); }
  const std::vector<uint8_t>& frame(Gpu& gpu, bool updated, POINT position, HCURSOR shape, StageTimes& times) {
    const auto began = Clock::now(); double convertMs = 0;
    if (updated) {
      gpu.context->CopyResource(gpu.staging.Get(), gpu.frame.Get()); D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(gpu.context->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark readback");
      const auto converting = Clock::now();
      host::capture::convertBlockToNv12(nv12_, width_, height_, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, {0, 0, width_, height_});
      convertMs = millis(Clock::now() - converting); gpu.context->Unmap(gpu.staging.Get(), 0);
    }
    const auto readback = Clock::now();
    CURSORINFO info{sizeof(CURSORINFO)}; (void)GetCursorInfo(&info);
    ICONINFO icon{}; require(GetIconInfo(shape, &icon) != FALSE, "Benchmark cursor");
    const host::capture::CursorDraw draw{shape, int(position.x) - int(icon.xHotspot), int(position.y) - int(icon.yHotspot), std::max(1, GetSystemMetrics(SM_CXCURSOR)), std::max(1, GetSystemMetrics(SM_CYCURSOR))};
    patch_.apply(nv12_, width_, height_, std::optional{draw}, [&](const host::capture::PixelRect& rect, auto&& visit) {
      if (const auto overlap = host::capture::intersect(rect, {0, 0, width_, height_})) {
        D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(gpu.context->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark readback");
        visit(static_cast<const uint8_t*>(mapped.pData) + size_t(overlap->top) * mapped.RowPitch + size_t(overlap->left) * 4, size_t(mapped.RowPitch), *overlap);
        gpu.context->Unmap(gpu.staging.Get(), 0);
      }
    }, screen_);
    if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor);
    const auto ended = Clock::now();
    times = {millis(readback - began) - convertMs, 0, millis(ended - readback), convertMs};
    return nv12_;
  }
 private:
  int width_, height_; HDC screen_ = nullptr; host::capture::Nv12CursorPatch patch_; std::vector<uint8_t> nv12_;
};
// capture-dirty-v1 (capture_dirty.hpp): the direct path as Capture::Modern runs
// it on a WARP device. The staging texture is persistent; a fresh frame copies
// only the scene's dirty rectangles plus the sweep band (CopySubresourceRegion)
// and converts only them. `activeSource` supplies what WGC would report.
const SceneSource* activeSource = nullptr;
class DirtyCapture {
 public:
  DirtyCapture(int width, int height) : width_(width), height_(height), nv12_(size_t(width) * height * 3 / 2) {
    screen_ = GetDC(nullptr); require(patch_.reserve(std::max(64, GetSystemMetrics(SM_CXCURSOR) + 2), std::max(64, GetSystemMetrics(SM_CYCURSOR) + 2), screen_), "Benchmark cursor canvas");
  }
  ~DirtyCapture() { ReleaseDC(nullptr, screen_); }
  const std::vector<uint8_t>& frame(Gpu& gpu, bool updated, POINT position, HCURSOR shape, StageTimes& times) {
    const auto began = Clock::now(); double convertMs = 0;
    if (updated) {
      require(activeSource != nullptr, "Benchmark scene source");
      const auto plan = host::capture::planDirtyCopy({stagingComplete_, true, activeSource->lastDirty, width_, height_, 0, sweepIndex_, policy_});
      std::vector<host::capture::PixelRect> pieces;
      if (plan.full) pieces.push_back({0, 0, width_, height_}); else { pieces = plan.rects; if (plan.sweep) pieces.push_back(*plan.sweep); ++sweepIndex_; }
      if (plan.full) gpu.context->CopyResource(gpu.staging.Get(), gpu.frame.Get());
      else for (const auto& r : pieces) { const D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1}; gpu.context->CopySubresourceRegion(gpu.staging.Get(), 0, UINT(r.left), UINT(r.top), 0, gpu.frame.Get(), 0, &box); }
      stagingComplete_ = true;
      D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(gpu.context->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark readback");
      const auto converting = Clock::now();
      for (const auto& r : pieces) host::capture::convertBlockToNv12(nv12_, width_, height_, static_cast<const uint8_t*>(mapped.pData) + size_t(r.top) * mapped.RowPitch + size_t(r.left) * 4, mapped.RowPitch, r);
      convertMs = millis(Clock::now() - converting); gpu.context->Unmap(gpu.staging.Get(), 0);
      if (plan.full) ++fullCopies; else ++partialCopies;
    }
    const auto readback = Clock::now();
    CURSORINFO info{sizeof(CURSORINFO)}; (void)GetCursorInfo(&info);
    ICONINFO icon{}; require(GetIconInfo(shape, &icon) != FALSE, "Benchmark cursor");
    const host::capture::CursorDraw draw{shape, int(position.x) - int(icon.xHotspot), int(position.y) - int(icon.yHotspot), std::max(1, GetSystemMetrics(SM_CXCURSOR)), std::max(1, GetSystemMetrics(SM_CYCURSOR))};
    patch_.apply(nv12_, width_, height_, std::optional{draw}, [&](const host::capture::PixelRect& rect, auto&& visit) {
      if (const auto overlap = host::capture::intersect(rect, {0, 0, width_, height_})) {
        D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(gpu.context->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark readback");
        visit(static_cast<const uint8_t*>(mapped.pData) + size_t(overlap->top) * mapped.RowPitch + size_t(overlap->left) * 4, size_t(mapped.RowPitch), *overlap);
        gpu.context->Unmap(gpu.staging.Get(), 0);
      }
    }, screen_);
    if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor);
    const auto ended = Clock::now();
    times = {millis(readback - began) - convertMs, 0, millis(ended - readback), convertMs};
    return nv12_;
  }
  static inline int fullCopies = 0, partialCopies = 0;
 private:
  int width_, height_; HDC screen_ = nullptr; host::capture::Nv12CursorPatch patch_; std::vector<uint8_t> nv12_;
  host::capture::DirtyPolicy policy_; bool stagingComplete_ = false; int sweepIndex_ = 0;
};
#endif
// Scaled capture (encoder smaller than the owned output): the pre-change
// Capture::Modern::render + Capture::frame sequence (row copy on a new frame,
// StretchDIBits HALFTONE of the whole surface on EVERY tick, DrawIconEx,
// conversion cache, bgraToNv12) ...
class LegacyScaledCapture {
 public:
  LegacyScaledCapture(int sw, int sh, int tw, int th) : sw_(sw), sh_(sh), tw_(tw), th_(th), dib_(tw, th), surface_(size_t(sw) * sh * 4), nv12_(size_t(tw) * th * 3 / 2) {}
  const std::vector<uint8_t>& frame(Gpu& gpu, bool updated, POINT position, HCURSOR shape, StageTimes& times) {
    const auto began = Clock::now();
    if (updated) readback(gpu, surface_, sw_, sh_);
    const auto readDone = Clock::now();
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = sw_; info.bmiHeader.biHeight = -sh_; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    require(StretchDIBits(dib_.dc, 0, 0, tw_, th_, 0, 0, sw_, sh_, surface_.data(), &info, DIB_RGB_COLORS, SRCCOPY) != 0, "Benchmark StretchDIBits");
    const auto composed = Clock::now();
    const auto stamp = drawCursor(dib_.dc, position, shape, sw_, sh_, tw_, th_, nullptr);
    GdiFlush();
    const auto cursor = Clock::now();
    if (!cache_.reuse(dib_.pixels, size_t(tw_) * th_ * 4, !updated && stampValid_ && stamp == stamp_)) host::bgraToNv12(dib_.pixels, nv12_.data(), tw_, th_);
    stamp_ = stamp; stampValid_ = true;
    times = {millis(readDone - began), millis(composed - readDone), millis(cursor - composed), millis(Clock::now() - cursor)};
    return nv12_;
  }
  static void readback(Gpu& gpu, std::vector<uint8_t>& surface, int sw, int sh) {
    gpu.context->CopyResource(gpu.staging.Get(), gpu.frame.Get()); D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(gpu.context->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark readback");
    for (int y = 0; y < sh; ++y) std::memcpy(surface.data() + size_t(y) * sw * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(sw) * 4);
    gpu.context->Unmap(gpu.staging.Get(), 0);
  }
  // Same cursor query/geometry as Capture::frame's DIB path; `save` receives
  // the destination rectangle before DrawIconEx writes it.
  template<class Save> static std::array<uint64_t, 6> drawCursor(HDC dc, POINT position, HCURSOR shape, int sw, int sh, int tw, int th, Save save) {
    CURSORINFO info{sizeof(CURSORINFO)}; (void)GetCursorInfo(&info);
    ICONINFO icon{}; require(GetIconInfo(shape, &icon) != FALSE, "Benchmark cursor");
    const int x = (position.x - int(icon.xHotspot)) * tw / sw, y = (position.y - int(icon.yHotspot)) * th / sh;
    const int cw = std::max(1, GetSystemMetrics(SM_CXCURSOR) * tw / sw), ch = std::max(1, GetSystemMetrics(SM_CYCURSOR) * th / sh);
    if constexpr (!std::is_same_v<Save, std::nullptr_t>) save(x, y, cw, ch);
    require(DrawIconEx(dc, x, y, shape, cw, ch, 0, nullptr, DI_NORMAL) != FALSE, "Benchmark cursor draw");
    if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor);
    return {uint64_t(reinterpret_cast<uintptr_t>(shape)), uint64_t(uint32_t(x)), uint64_t(uint32_t(y)), uint64_t(cw), uint64_t(ch), 1};
  }
 private:
  int sw_, sh_, tw_, th_; Dib dib_; std::vector<uint8_t> surface_, nv12_; host::PixelConversionCache cache_; std::array<uint64_t, 6> stamp_{}; bool stampValid_ = false;
};
#ifdef MULTITAP_AREA_SCALE
// ... and the new sequence: AreaScaler only for a new frame, the pixels under
// last tick's cursor put back instead of re-filtering the whole surface.
class AreaScaledCapture {
 public:
  AreaScaledCapture(int sw, int sh, int tw, int th, bool incremental = true) : sw_(sw), sh_(sh), tw_(tw), th_(th), dib_(tw, th), surface_(size_t(sw) * sh * 4), nv12_(size_t(tw) * th * 3 / 2), scaler_(sw, sh, tw, th), incremental_(incremental) {}
  const std::vector<uint8_t>& frame(Gpu& gpu, bool updated, POINT position, HCURSOR shape, StageTimes& times) {
    const auto began = Clock::now();
    if (!incremental_) first_ = true;  // reference: resample the whole surface every tick
    if (saved_) { GdiFlush(); for (int r = 0; r < saved_->height(); ++r) std::memcpy(dib_.pixels + (size_t(saved_->top + r) * tw_ + saved_->left) * 4, backup_.data() + size_t(r) * saved_->width() * 4, size_t(saved_->width()) * 4); saved_.reset(); }
    if (updated) LegacyScaledCapture::readback(gpu, surface_, sw_, sh_);
    const auto readDone = Clock::now();
    GdiFlush();
    if (updated || first_) { scaler_.scale(surface_.data(), size_t(sw_) * 4, dib_.pixels, size_t(tw_) * 4); first_ = false; }
    const auto composed = Clock::now();
    const auto stamp = LegacyScaledCapture::drawCursor(dib_.dc, position, shape, sw_, sh_, tw_, th_, [&](int x, int y, int w, int h) {
      const int left = std::max(0, x), top = std::max(0, y), right = std::min(tw_, x + w), bottom = std::min(th_, y + h);
      if (left >= right || top >= bottom) return;
      saved_ = host::capture::PixelRect{left, top, right, bottom}; backup_.resize(size_t(right - left) * (bottom - top) * 4);
      for (int r = top; r < bottom; ++r) std::memcpy(backup_.data() + size_t(r - top) * (right - left) * 4, dib_.pixels + (size_t(r) * tw_ + left) * 4, size_t(right - left) * 4);
    });
    GdiFlush();
    const auto cursor = Clock::now();
    if (!cache_.reuse(dib_.pixels, size_t(tw_) * th_ * 4, !updated && stampValid_ && stamp == stamp_)) host::bgraToNv12(dib_.pixels, nv12_.data(), tw_, th_);
    stamp_ = stamp; stampValid_ = true;
    times = {millis(readDone - began), millis(composed - readDone), millis(cursor - composed), millis(Clock::now() - cursor)};
    return nv12_;
  }
 private:
  int sw_, sh_, tw_, th_; Dib dib_; std::vector<uint8_t> surface_, nv12_; host::capture::AreaScaler scaler_; host::PixelConversionCache cache_;
  bool incremental_; std::array<uint64_t, 6> stamp_{}; bool stampValid_ = false, first_ = true; std::optional<host::capture::PixelRect> saved_; std::vector<uint8_t> backup_;
};
struct FullAreaScaledCapture : AreaScaledCapture { FullAreaScaledCapture(int sw, int sh, int tw, int th) : AreaScaledCapture(sw, sh, tw, th, false) {} };
#endif
// One scaled case: per-tick capture-side wall time (readback + compose +
// cursor + convert), no encoder (unchanged by this path). Returns the NV12s.
struct ScaledResult { std::vector<std::vector<uint8_t>> samples; std::vector<uint64_t> digests; };
template<class Capture>
ScaledResult runScaledCase(const char* pipeline, int sw, int sh, int tw, int th, Scene scene, int frames, bool forceWarp, const SceneSource& source) {
  constexpr int warmup = 30;
  Gpu gpu(sw, sh, forceWarp); Capture capture(sw, sh, tw, th);
  const HCURSOR shapes[] = {LoadCursorW(nullptr, IDC_ARROW), LoadCursorW(nullptr, IDC_IBEAM), LoadCursorW(nullptr, IDC_HAND)};
  Series readback, compose, cursor, convert, total; std::vector<uint8_t> bgra; ScaledResult outputs;
  for (int n = -warmup; n < frames; ++n) {
    const int index = n + warmup;
    const bool updated = source.render(scene, index, bgra); if (updated) gpu.upload(bgra);
    const auto position = source.cursor(scene, index); const HCURSOR shape = shapes[scene == Scene::still ? 0 : (index / 40) % 3];
    StageTimes times; const auto began = Clock::now();
    const auto& nv12 = capture.frame(gpu, updated, position, shape, times);
    const auto ended = Clock::now();
    if (n >= 0) { readback.add(times.readback); compose.add(times.compose); cursor.add(times.cursor); convert.add(times.convert); total.add(millis(ended - began)); if (n % 37 == 0) outputs.samples.push_back(nv12); }
    outputs.digests.push_back(digest(nv12));
  }
  std::cout << std::fixed << std::setprecision(3) << "{\"pipeline\":\"" << pipeline << "\",\"source\":\"" << sw << 'x' << sh << "\",\"encoded\":\"" << tw << 'x' << th << "\",\"scene\":\"" << sceneName(scene)
            << "\",\"device\":\"" << (gpu.warp ? "warp" : "hardware") << "\",\"frames\":" << frames << ",\"stagesMs\":{\"readback\":"; readback.json(std::cout);
  std::cout << ",\"scale\":"; compose.json(std::cout); std::cout << ",\"cursor\":"; cursor.json(std::cout); std::cout << ",\"convert\":"; convert.json(std::cout);
  std::cout << "},\"captureTickMs\":"; total.json(std::cout); std::cout << ",\"captureTickMaxMs\":" << total.quantile(1.0) << '}' << std::endl;
  return outputs;
}
double lumaPsnr(const std::vector<std::vector<uint8_t>>& a, const std::vector<std::vector<uint8_t>>& b, int width, int height) {
  double error = 0; size_t count = 0;
  for (size_t f = 0; f < std::min(a.size(), b.size()); ++f) for (size_t i = 0; i < size_t(width) * height; ++i) { const double d = double(a[f][i]) - double(b[f][i]); error += d * d; ++count; }
  return count ? 10 * std::log10(255.0 * 255.0 / std::max(1e-9, error / double(count))) : 0;
}
struct CaseResult { std::vector<uint64_t> digests; };
template<class Capture>
CaseResult runCase(const char* pipeline, int width, int height, Scene scene, int frames, bool forceWarp, const SceneSource& source) {
  constexpr int warmup = 30, fps = 60;
  const int bitrate = width == 1280 ? 6000000 : width == 1920 ? 12000000 : 20000000;
  Gpu gpu(width, height, forceWarp); Capture capture(width, height);
  // --x264: the software encoder of a GPU-less VM (the primary case) also on a PC with a GPU.
  host::Encoder encoder(width, height, fps, bitrate, host::EncoderTuning{}, forceX264 ? host::EncoderBackend::x264 : host::EncoderBackend::automatic); host::FrameEncodePolicy policy(size_t(width) * height * 3 / 2);
  const HCURSOR shapes[] = {LoadCursorW(nullptr, IDC_ARROW), LoadCursorW(nullptr, IDC_IBEAM), LoadCursorW(nullptr, IDC_HAND)};
  Series readback, compose, cursor, convert, prepare, encode, total, captureCpu, policyCpu, encodeCpu, totalCpu; std::vector<uint8_t> bgra; size_t submitted = 0, encoded = 0, bytes = 0; CaseResult result;
  uint64_t pts = 0; activeSource = &source;
  const auto emit = [&](host::Frame frame) { require(policy.output(frame.ptsUs, frame.key, pts), "Benchmark output PTS"); ++encoded; bytes += host::mediaPacket(1, frame.ptsUs, frame.bytes, frame.key).size(); };
  double cpuBegan = 0; Clock::time_point wallBegan{};
  for (int n = -warmup; n < frames; ++n) {
    const int index = n + warmup;
    if (n == 0) { cpuBegan = cpuMs(); wallBegan = Clock::now(); }
    const bool updated = source.render(scene, index, bgra); if (updated) gpu.upload(bgra);
    const auto position = source.cursor(scene, index); const HCURSOR shape = shapes[scene == Scene::still ? 0 : (index / 40) % 3];
    pts = uint64_t(index) * 16667 + 1;
    const auto c0 = cycles(); const auto began = Clock::now(); StageTimes times;
    const auto& nv12 = capture.frame(gpu, updated, position, shape, times);
    const auto prepared = Clock::now(); const auto c1 = cycles(); const auto decision = policy.prepare(nv12, pts); const auto encodeBegan = Clock::now(); const auto c2 = cycles();
    if (decision.submit) { encoder.encode(nv12.data(), pts, emit); if (n >= 0) ++submitted; }
    const auto ended = Clock::now(); const auto c3 = cycles();
    if (n >= 0) {
      readback.add(times.readback); compose.add(times.compose); cursor.add(times.cursor); convert.add(times.convert);
      prepare.add(millis(encodeBegan - prepared)); encode.add(millis(ended - encodeBegan)); total.add(millis(ended - began));
      captureCpu.add(cpuOf(c0, c1)); policyCpu.add(cpuOf(c1, c2)); encodeCpu.add(cpuOf(c2, c3)); totalCpu.add(cpuOf(c0, c3));
    }
    result.digests.push_back(digest(nv12));
  }
  const double cpu = cpuMs() - cpuBegan, wall = millis(Clock::now() - wallBegan);
  encoder.finish(emit);
  std::cout << std::fixed << std::setprecision(3) << "{\"pipeline\":\"" << pipeline << "\",\"width\":" << width << ",\"height\":" << height << ",\"scene\":\"" << sceneName(scene)
            << "\",\"device\":\"" << (gpu.warp ? "warp" : "hardware") << "\",\"encoder\":\"" << encoder.backendName() << "\",\"frames\":" << frames << ",\"submitted\":" << submitted << ",\"encodedTotal\":" << encoded << ",\"bitrateRequested\":" << bitrate
            << ",\"stagesMs\":{\"readback\":"; readback.json(std::cout); std::cout << ",\"compose\":"; compose.json(std::cout); std::cout << ",\"cursor\":"; cursor.json(std::cout);
  std::cout << ",\"convert\":"; convert.json(std::cout); std::cout << ",\"policy\":"; prepare.json(std::cout); std::cout << ",\"encode\":"; encode.json(std::cout);
  std::cout << "},\"totalMs\":"; total.json(std::cout);
  std::cout << ",\"processCpuMs\":{\"captureSide\":"; captureCpu.json(std::cout); std::cout << ",\"policy\":"; policyCpu.json(std::cout); std::cout << ",\"encode\":"; encodeCpu.json(std::cout);
  std::cout << ",\"total\":"; totalCpu.json(std::cout); std::cout << '}';
  std::cout << ",\"impliedMaxFps\":" << 1000.0 / std::max(0.001, total.mean()) << ",\"cpuMsPerFrame\":" << cpu / frames << ",\"wallMsPerFrame\":" << wall / frames
            << ",\"nv12SequenceDigest\":\"" << std::hex << std::accumulate(result.digests.begin(), result.digests.end(), uint64_t(0), [](uint64_t a, uint64_t b) { return (a ^ b) * 0x100000001b3ull; }) << std::dec << "\"}" << std::endl;
  return result;
}
// Encoder wrapper buffer handling only (no MFT): the previous per-frame
// MFCreateMemoryBuffer for the NV12 input plus two output workspaces per
// encode (output + NEED_MORE_INPUT probe), versus the reused buffers.
void encoderBuffers(int width, int height, int samples) {
  const DWORD count = DWORD(width * height * 3 / 2), workspace = 2 * 1024 * 1024; std::vector<uint8_t> nv12(count, 77);
  Series fresh, reused; host::Com<IMFMediaBuffer> pooled, output; host::check(MFCreateMemoryBuffer(count, &pooled), "Pool"); host::check(MFCreateMemoryBuffer(workspace, &output), "Pool");
  for (int i = 0; i < samples + 10; ++i) {
    const auto a = Clock::now();
    { host::Com<IMFMediaBuffer> buffer; host::check(MFCreateMemoryBuffer(count, &buffer), "Fresh"); BYTE* p = nullptr; buffer->Lock(&p, nullptr, nullptr); std::memcpy(p, nv12.data(), count); buffer->Unlock(); buffer->SetCurrentLength(count);
      for (int k = 0; k < 2; ++k) { host::Com<IMFMediaBuffer> out; host::check(MFCreateMemoryBuffer(workspace, &out), "Fresh"); BYTE* q = nullptr; out->Lock(&q, nullptr, nullptr); if (k == 0) std::memset(q, 1, 32768); out->Unlock(); } }
    const auto b = Clock::now();
    { BYTE* p = nullptr; pooled->Lock(&p, nullptr, nullptr); std::memcpy(p, nv12.data(), count); pooled->Unlock(); pooled->SetCurrentLength(count);
      for (int k = 0; k < 2; ++k) { output->SetCurrentLength(0); BYTE* q = nullptr; output->Lock(&q, nullptr, nullptr); if (k == 0) std::memset(q, 1, 32768); output->Unlock(); } }
    const auto c = Clock::now();
    if (i >= 10) { fresh.add(millis(b - a)); reused.add(millis(c - b)); }
  }
  std::cout << std::fixed << std::setprecision(3) << "{\"stage\":\"encoder-wrapper-buffers\",\"width\":" << width << ",\"height\":" << height << ",\"samples\":" << samples << ",\"perFrameAllocationMs\":"; fresh.json(std::cout);
  std::cout << ",\"reusedBuffersMs\":"; reused.json(std::cout); std::cout << ",\"encodedOutputBytesTouched\":32768}" << std::endl;
}
void topology(int samples) {
  Series monitors, settings, desktop; std::string state = "ok";
  try {
    const auto observed = host::currentCaptureMonitors(); host::CaptureSelection selection; selection.bounds = observed.front().bounds; selection.monitors = {observed.front()};
    for (int i = 0; i < samples; ++i) {
      const auto began = Clock::now(); host::validateCaptureSelection(selection, host::currentCaptureMonitors()); const auto validated = Clock::now();
      DEVMODEW mode{}; mode.dmSize = sizeof(mode); require(EnumDisplaySettingsExW(observed.front().name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE, "EnumDisplaySettingsExW");
      const auto ended = Clock::now(); (void)host::interactiveDesktopAvailable(); const auto checked = Clock::now();
      monitors.add(millis(validated - began)); settings.add(millis(ended - validated)); desktop.add(millis(checked - ended));
    }
  } catch (const std::exception&) { state = "unavailable"; }
  std::cout << std::fixed << std::setprecision(3) << "{\"stage\":\"per-frame-topology-check\",\"state\":\"" << state << "\",\"samples\":" << monitors.values.size() << ",\"validateCaptureSelectionMs\":"; monitors.json(std::cout);
  std::cout << ",\"enumDisplaySettingsMs\":"; settings.json(std::cout); std::cout << ",\"interactiveDesktopCheckMs\":"; desktop.json(std::cout); std::cout << ",\"readOnly\":true}" << std::endl;
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    int frames = 240, cpus = 0; bool forceWarp = false, scaledOnly = false;
    for (int i = 1; i < argc; ++i) { const std::wstring arg = argv[i]; if (arg == L"--warp") forceWarp = true; else if (arg == L"--x264") forceX264 = true; else if (arg == L"--scaled") scaledOnly = true; else if (arg == L"--cpus" && i + 1 < argc) cpus = std::stoi(argv[++i]); else frames = std::stoi(arg); }
    require(frames >= 30 && frames <= 2000 && cpus >= 0 && cpus <= 64, "Usage: MultitapHostPipelineBenchmark [FRAMES] [--warp] [--x264] [--cpus N] [--scaled]");
    // Optional affinity limit for this benchmark process only. The MFT/WARP still
    // size their thread pools from the machine's CPU count, so this is NOT a
    // faithful small-VM proxy; it is kept only for diagnostic comparisons.
    if (cpus) require(SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR(1) << cpus) - 1) != FALSE, "Benchmark affinity");
    host::Runtime runtime;
    std::cout << "{\"kind\":\"host-pipeline-benchmark\",\"syntheticFrames\":true,\"desktopPixelsRead\":false,\"pixelsSaved\":false,\"network\":false,\"warpForced\":" << (forceWarp ? "true" : "false")
              << ",\"affinityCpus\":" << cpus << ",\"tscCyclesPerMs\":" << std::fixed << std::setprecision(0) << cyclesPerMs() << "}" << std::endl;
    // Scaled capture: field shapes where the client limit is below the owned output.
    for (const auto [sw, sh, tw, th] : {std::tuple{2560, 1440, 1920, 1080}, std::tuple{2560, 1350, 1920, 1012}, std::tuple{1920, 1080, 1280, 720}}) {
      const SceneSource source(sw, sh);
      for (const auto scene : {Scene::motion, Scene::partial, Scene::cursor, Scene::still}) {
        const auto legacy = runScaledCase<LegacyScaledCapture>("scaled-halftone", sw, sh, tw, th, scene, frames, forceWarp, source);
#ifdef MULTITAP_AREA_SCALE
        const auto area = runScaledCase<AreaScaledCapture>("scaled-area", sw, sh, tw, th, scene, frames, forceWarp, source);
        // Incremental (resample only new frames, restore under the cursor) must
        // equal resampling the whole surface on every tick, byte for byte.
        const auto full = runScaledCase<FullAreaScaledCapture>("scaled-area-every-tick", sw, sh, tw, th, scene, frames, forceWarp, source);
        require(area.digests == full.digests, "Incremental scaled capture differs from a full resample");
        std::cout << std::fixed << std::setprecision(2) << "{\"stage\":\"scaled-quality\",\"source\":\"" << sw << 'x' << sh << "\",\"scene\":\"" << sceneName(scene) << "\",\"incrementalEqualsFull\":true,\"areaVsHalftoneLumaPsnrDb\":" << lumaPsnr(area.samples, legacy.samples, tw, th) << '}' << std::endl;
#endif
      }
    }
    if (scaledOnly) return 0;
    topology(200);
    for (const auto [width, height] : {std::pair{1280, 720}, std::pair{1920, 1080}, std::pair{2560, 1440}}) {
#ifdef MULTITAP_DIRECT_CAPTURE
      encoderBuffers(width, height, 200);
#endif
      const SceneSource source(width, height);
      for (const auto scene : {Scene::motion, Scene::partial, Scene::cursor, Scene::still}) {
        const auto legacy = runCase<LegacyCapture>("legacy", width, height, scene, frames, forceWarp, source);
#ifdef MULTITAP_DIRECT_CAPTURE
        // Same scene, same encoder input requirement: every frame's final NV12
        // must be byte-identical to the legacy DIB composition.
        const auto direct = runCase<DirectCapture>("direct", width, height, scene, frames, forceWarp, source);
        require(direct.digests == legacy.digests, "Direct capture NV12 differs from the legacy composition");
        // capture-dirty-v1: only the changed rectangles (plus the sweep band)
        // are read back and converted; every frame's NV12 must still be the
        // legacy composition byte for byte.
        DirtyCapture::fullCopies = DirtyCapture::partialCopies = 0;
        const auto dirty = runCase<DirtyCapture>("direct-dirty", width, height, scene, frames, forceWarp, source);
        require(dirty.digests == legacy.digests, "Dirty-rectangle capture NV12 differs from the legacy composition");
        std::cout << "{\"stage\":\"dirty-copies\",\"width\":" << width << ",\"height\":" << height << ",\"scene\":\"" << sceneName(scene) << "\",\"fullCopies\":" << DirtyCapture::fullCopies << ",\"partialCopies\":" << DirtyCapture::partialCopies << ",\"nv12EqualsLegacy\":true}" << std::endl;
#endif
      }
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
