// Readback cost of the CPU capture path, measured with a real Windows Graphics
// Capture of this benchmark's OWN window (screen-sized, never activated,
// click-through, 1/255 opaque on screen; no other window or desktop pixel is
// captured, nothing is saved or sent).
//
// Per delivered frame, on the device the product would use (hardware, else
// WARP; --warp forces WARP, the field case of a display composed by the Basic
// Render Driver):
//  - the released path: CopyResource of the whole pool texture into a staging
//    texture, Map, convert the whole frame to NV12;
//  - capture-dirty-v1 (capture_dirty.hpp): CopySubresourceRegion of the
//    frame's dirty rectangles into a persistent staging texture, Map, convert
//    only those rectangles;
//  - a GPU-side copy of the pool texture into an owned texture, waited for
//    (reading the pool texture without a CPU map);
// and a byte comparison of both staging textures and both NV12 frames: the
// dirty path must equal the full path exactly, also after the loop stalled
// while the window changed several times (frames the two-buffer pool dropped).
// Scenes: a box moving over document-like text (partial), a 400x20 text row
// (typing), the whole window repainted with a new tint (full).
#include "capture_dirty.hpp"
#include "capture_session_interval.hpp"
#include "media.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using host::capture::PixelRect;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
double ms(Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); }
struct Series {
  std::vector<double> values;
  void add(double value) { values.push_back(value); }
  double quantile(double q) const { if (values.empty()) return 0; auto sorted = values; std::sort(sorted.begin(), sorted.end()); return sorted[std::min(sorted.size() - 1, size_t(q * double(sorted.size() - 1) + 0.5))]; }
  double mean() const { return values.empty() ? 0 : std::accumulate(values.begin(), values.end(), 0.0) / double(values.size()); }
  void json(std::ostream& out) const { out << "{\"n\":" << values.size() << ",\"mean\":" << mean() << ",\"p50\":" << quantile(.5) << ",\"p95\":" << quantile(.95) << ",\"max\":" << quantile(1) << '}'; }
};

// The window's content: document-like text rows in a memory DIB, a box and a
// text row that change per trial, and a tint for whole-window repaints.
struct Content {
  int width = 0, height = 0; HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr; uint8_t* pixels = nullptr;
  std::mutex lock; RECT box{}; int tint = 0; uint32_t rowSeed = 1;
  void create(int w, int h) {
    width = w; height = h; dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0); require(dc && bitmap && pixels, "Content DIB"); previous = SelectObject(dc, bitmap);
    paintDocument();
  }
  ~Content() { if (dc && previous) SelectObject(dc, previous); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
  void paintDocument() {
    uint32_t seed = 12345; auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (size_t i = 0; i < size_t(width) * height * 4; i += 4) { pixels[i] = uint8_t(250 - tint); pixels[i + 1] = uint8_t(248 - tint / 2); pixels[i + 2] = 245; pixels[i + 3] = 255; }
    for (int line = 0; line * 18 + 14 < height; ++line) {
      int x = 24 + int(next() % 40);
      while (x + 9 < width - 24) {
        const int glyph = 5 + int(next() % 4);
        for (int y = line * 18 + 3; y < line * 18 + 14; ++y) for (int dx = 0; dx < glyph; ++dx) if ((next() & 3) != 0) { auto* p = pixels + (size_t(y) * width + x + dx) * 4; p[0] = 30; p[1] = 30; p[2] = 40; }
        x += glyph + 2 + ((next() % 9) == 0 ? 7 : 0);
      }
    }
  }
  // The box: 160x160 at a trial-dependent place.
  RECT boxAt(int trial) const { const int size = std::min(160, height / 4); const int x = 40 + (trial * 97) % std::max(1, width - size - 80), y = 60 + (trial * 61) % std::max(1, height - size - 120); return {x, y, x + size, y + size}; }
  RECT textRow() const { return {width / 8, height / 3, width / 8 + 400, height / 3 + 20}; }
};
Content content;
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
    { std::lock_guard<std::mutex> guard(content.lock);
      const RECT& r = paint.rcPaint; BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, content.dc, r.left, r.top, SRCCOPY);
      HBRUSH brush = CreateSolidBrush(RGB(40 + content.tint, 90, 200 - content.tint)); FillRect(dc, &content.box, brush); DeleteObject(brush);
      // The text row: glyph blocks from the row seed.
      const RECT row = content.textRow(); HBRUSH ink = CreateSolidBrush(RGB(20, 20, 30)); uint32_t seed = content.rowSeed;
      for (int x = row.left; x + 8 < row.right; x += 10) { seed = seed * 1664525u + 1013904223u; if (seed & 0x100) { RECT g{x, row.top + 4, x + 6, row.bottom - 4}; FillRect(dc, &g, ink); } }
      DeleteObject(ink); }
    EndPaint(window, &paint); return 0;
  }
  return DefWindowProcW(window, message, w, l);
}

struct Readback {
  host::Com<ID3D11Device> device; host::Com<ID3D11DeviceContext> context; std::string adapter; bool warp = false;
  host::Com<ID3D11Texture2D> stagingFull, stagingDirty, gpuCopy; host::Com<ID3D11Query> query;
  int width = 0, height = 0;
  Readback(bool forceWarp, int w, int h) : width(w), height(h) {
    HRESULT hr = forceWarp ? E_FAIL : D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) { warp = true; host::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device"); }
    host::Com<IDXGIDevice> dxgi; host::Com<IDXGIAdapter> found; DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&found)) && SUCCEEDED(found->GetDesc(&desc))) for (wchar_t c : std::wstring(desc.Description)) adapter += (c >= 32 && c < 127) ? char(c) : '?';
    D3D11_TEXTURE2D_DESC t{}; t.Width = UINT(w); t.Height = UINT(h); t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_B8G8R8A8_UNORM; t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_STAGING; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    host::check(device->CreateTexture2D(&t, nullptr, &stagingFull), "Staging"); host::check(device->CreateTexture2D(&t, nullptr, &stagingDirty), "Staging");
    t.Usage = D3D11_USAGE_DEFAULT; t.CPUAccessFlags = 0; t.BindFlags = D3D11_BIND_SHADER_RESOURCE; host::check(device->CreateTexture2D(&t, nullptr, &gpuCopy), "GPU copy");
    D3D11_QUERY_DESC q{}; q.Query = D3D11_QUERY_EVENT; host::check(device->CreateQuery(&q, &query), "Query");
  }
  void waitGpu() { context->End(query.Get()); context->Flush(); while (context->GetData(query.Get(), nullptr, 0, 0) == S_FALSE) SwitchToThread(); }
};
struct Scene { const char* name; };
struct Stats {
  Series readbackFull, convertFull, readbackDirty, convertDirty, gpuCopy, dirtyRects, dirtyAreaPercent, framesPerTick;
  int frames = 0, fullPlans = 0, unknownDirty = 0, stagingMismatch = 0, nv12Mismatch = 0, missed = 0, stalledTicks = 0;
  bool diverged = false;
};
}

int wmain(int argc, wchar_t** argv) try {
  bool forceWarp = false; int trials = 120, w = 0, h = 0;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--warp") forceWarp = true;
    else if (arg == L"--trials" && i + 1 < argc) trials = std::stoi(argv[++i]);
    else if (arg == L"--size" && i + 2 < argc) { w = std::stoi(argv[++i]); h = std::stoi(argv[++i]); }
    else throw std::runtime_error("Usage: MultitapHostCaptureReadbackBenchmark [--warp] [--trials N] [--size W H]");
  }
  require(trials >= 10 && trials <= 2000, "Invalid trials");
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  if (!w || !h) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }
  w &= ~1; h &= ~1; require(w >= 64 && h >= 64, "Window size");
  content.create(w, h); content.box = content.boxAt(0);
  std::atomic<HWND> window{nullptr};
  std::thread ui([&] {
    WNDCLASSW type{}; type.lpfnWndProc = windowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapReadbackBenchmark"; RegisterClassW(&type);
    HWND created = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, type.lpszClassName, L"Multitap readback benchmark", WS_POPUP, 0, 0, w, h, nullptr, nullptr, type.hInstance, nullptr);
    SetLayeredWindowAttributes(created, 0, 1, LWA_ALPHA);
    ShowWindow(created, SW_SHOWNOACTIVATE); UpdateWindow(created); window = created;
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  });
  while (!window.load()) Sleep(1);

  using namespace winrt::Windows::Graphics::Capture;
  Readback rb(forceWarp, w, h);
  host::Com<IDXGIDevice> dxgi; host::check(rb.device.As(&dxgi), "DXGI device"); winrt::com_ptr<IInspectable> inspectable;
  host::check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()), "WinRT device");
  const auto winDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  GraphicsCaptureItem item{nullptr};
  const auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  host::check(factory->CreateForWindow(window.load(), winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)), "Capture own window");
  require(item.Size().Width == w && item.Size().Height == h, "Capture item size");
  HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(winDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
  auto arrived = pool.FrameArrived([signal](auto const&, auto const&) { SetEvent(signal); });
  auto session = pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(false); host::deliverEveryComposedFrame(session);
  bool dirtyMode = false;
  if (const auto dirty = session.try_as<IGraphicsCaptureSession4>()) { try { dirty.DirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly); dirtyMode = dirty.DirtyRegionMode() == GraphicsCaptureDirtyRegionMode::ReportOnly; } catch (...) {} }
  session.StartCapture();
  std::cout << std::fixed << std::setprecision(3) << "{\"kind\":\"capture-readback-benchmark\",\"ownWindowOnly\":true,\"pixelsSaved\":false,\"device\":\"" << (rb.warp ? "warp" : "hardware") << "\",\"adapter\":\"" << rb.adapter
            << "\",\"size\":\"" << w << 'x' << h << "\",\"dirtyRegionMode\":" << (dirtyMode ? "\"reportOnly\"" : "\"unavailable\"") << ",\"trials\":" << trials << ",\"build\":" << [] { OSVERSIONINFOEXW v{sizeof(v)}; using F = LONG(WINAPI*)(OSVERSIONINFOEXW*); if (const auto f = reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v); return v.dwBuildNumber; }() << '}' << std::endl;

  const PixelRect whole{0, 0, w, h};
  std::vector<uint8_t> nv12Full(size_t(w) * h * 3 / 2), nv12Dirty(nv12Full.size());
  bool stagingComplete = false; auto lastFull = Clock::now(); int sweepIndex = 0;
  host::capture::DirtyPolicy policy;
  // Consumes the frames in the pool: the released full path, the dirty path, the comparison.
  const auto consume = [&](Stats& stats, bool stalled, const char* scene) {
    if (WaitForSingleObject(signal, 500) != WAIT_OBJECT_0) { ++stats.missed; return; }
    Sleep(2);  // let a second frame of a stall arrive before draining (the product drains up to three per tick)
    ResetEvent(signal);
    Direct3D11CaptureFrame latest{nullptr}; std::vector<PixelRect> dirty; bool known = dirtyMode; int taken = 0;
    for (int n = 0; n < 8; ++n) {
      auto frame = pool.TryGetNextFrame(); if (!frame) break; ++taken;
      if (known) {
        if (const auto frame2 = frame.try_as<IDirect3D11CaptureFrame2>()) {
          try { for (const auto& r : frame2.DirtyRegions()) dirty.push_back({r.X, r.Y, r.X + r.Width, r.Y + r.Height}); } catch (...) { known = false; }
        } else known = false;
      }
      if (latest) latest.Close(); latest = std::move(frame);
    }
    if (!latest) { ++stats.missed; return; }
    if (stalled) ++stats.stalledTicks;
    stats.framesPerTick.add(taken); if (!known) ++stats.unknownDirty;
    const auto size = latest.ContentSize(); require(size.Width == w && size.Height == h, "Frame size");
    auto access = latest.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(); host::Com<ID3D11Texture2D> texture;
    host::check(access->GetInterface(IID_PPV_ARGS(&texture)), "Frame texture");
    // Released path.
    const auto t0 = Clock::now();
    rb.context->CopyResource(rb.stagingFull.Get(), texture.Get()); D3D11_MAPPED_SUBRESOURCE full{}; host::check(rb.context->Map(rb.stagingFull.Get(), 0, D3D11_MAP_READ, 0, &full), "Map full");
    const auto t1 = Clock::now();
    host::capture::convertBlockToNv12(nv12Full, w, h, static_cast<const uint8_t*>(full.pData), full.RowPitch, whole);
    const auto t2 = Clock::now();
    // Dirty path.
    const auto now = Clock::now();
    const auto plan = host::capture::planDirtyCopy({stagingComplete, known, dirty, w, h, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - lastFull).count()), sweepIndex, policy});
    std::vector<PixelRect> pieces = plan.rects; if (plan.sweep) pieces.push_back(*plan.sweep);
    const auto t3 = Clock::now();
    if (plan.full) { rb.context->CopyResource(rb.stagingDirty.Get(), texture.Get()); lastFull = now; stagingComplete = true; ++stats.fullPlans; }
    else { ++sweepIndex; for (const auto& r : pieces) { const D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1}; rb.context->CopySubresourceRegion(rb.stagingDirty.Get(), 0, UINT(r.left), UINT(r.top), 0, texture.Get(), 0, &box); } }
    D3D11_MAPPED_SUBRESOURCE part{}; host::check(rb.context->Map(rb.stagingDirty.Get(), 0, D3D11_MAP_READ, 0, &part), "Map dirty");
    const auto t4 = Clock::now();
    if (plan.full) host::capture::convertBlockToNv12(nv12Dirty, w, h, static_cast<const uint8_t*>(part.pData), part.RowPitch, whole);
    else for (const auto& r : pieces) host::capture::convertBlockToNv12(nv12Dirty, w, h, static_cast<const uint8_t*>(part.pData) + size_t(r.top) * part.RowPitch + size_t(r.left) * 4, part.RowPitch, r);
    const auto t5 = Clock::now();
    // GPU-side copy of the pool texture (no CPU map).
    const auto t6 = Clock::now(); rb.context->CopyResource(rb.gpuCopy.Get(), texture.Get()); rb.waitGpu(); const auto t7 = Clock::now();
    latest.Close();
    // Comparison: the dirty path's staging and NV12 must equal the full path's.
    bool same = true; PixelRect differ{w, h, 0, 0};
    for (int y = 0; y < h; ++y) {
      const auto* a = static_cast<const uint8_t*>(full.pData) + size_t(y) * full.RowPitch; const auto* b = static_cast<const uint8_t*>(part.pData) + size_t(y) * part.RowPitch;
      if (std::memcmp(a, b, size_t(w) * 4) == 0) continue;
      same = false; differ.top = std::min(differ.top, y); differ.bottom = y + 1;
      for (int x = 0; x < w; ++x) if (std::memcmp(a + size_t(x) * 4, b + size_t(x) * 4, 4) != 0) { differ.left = std::min(differ.left, x); differ.right = std::max(differ.right, x + 1); }
    }
    if (!same) {
      ++stats.stagingMismatch;
      // Where the dirty path's staging differs from the frame, against what this
      // tick was told: a divergence that began this tick names an under-report
      // (or a dropped frame); one carried over stays until a full copy.
      std::cout << "{\"mismatch\":{\"scene\":\"" << scene << "\",\"tick\":" << stats.frames << ",\"stalled\":" << (stalled ? "true" : "false") << ",\"framesTaken\":" << taken << ",\"known\":" << (known ? "true" : "false")
                << ",\"carriedOver\":" << (stats.diverged ? "true" : "false") << ",\"differs\":[" << differ.left << ',' << differ.top << ',' << differ.right << ',' << differ.bottom << "],\"reported\":[";
      for (size_t i = 0; i < dirty.size(); ++i) std::cout << (i ? "," : "") << '[' << dirty[i].left << ',' << dirty[i].top << ',' << dirty[i].right << ',' << dirty[i].bottom << ']';
      std::cout << "],\"planFull\":" << (plan.full ? "true" : "false") << "}}" << std::endl;
    }
    stats.diverged = !same;
    if (nv12Full != nv12Dirty) ++stats.nv12Mismatch;
    rb.context->Unmap(rb.stagingFull.Get(), 0); rb.context->Unmap(rb.stagingDirty.Get(), 0);
    ++stats.frames;
    stats.readbackFull.add(ms(t1 - t0)); stats.convertFull.add(ms(t2 - t1)); stats.readbackDirty.add(ms(t4 - t3)); stats.convertDirty.add(ms(t5 - t4)); stats.gpuCopy.add(ms(t7 - t6));
    stats.dirtyRects.add(double(plan.rects.size())); stats.dirtyAreaPercent.add(plan.full ? 100.0 : 100.0 * double(plan.area()) / (double(w) * h));
  };
  const auto report = [&](const char* scene, const Stats& s) {
    std::cout << "{\"scene\":\"" << scene << "\",\"frames\":" << s.frames << ",\"missed\":" << s.missed << ",\"stalledTicks\":" << s.stalledTicks << ",\"fullPlans\":" << s.fullPlans << ",\"unknownDirty\":" << s.unknownDirty
              << ",\"stagingMismatch\":" << s.stagingMismatch << ",\"nv12Mismatch\":" << s.nv12Mismatch
              << ",\"releasedMs\":{\"readback\":"; s.readbackFull.json(std::cout); std::cout << ",\"convert\":"; s.convertFull.json(std::cout);
    std::cout << "},\"dirtyMs\":{\"readback\":"; s.readbackDirty.json(std::cout); std::cout << ",\"convert\":"; s.convertDirty.json(std::cout);
    std::cout << "},\"gpuCopyWaitMs\":"; s.gpuCopy.json(std::cout); std::cout << ",\"dirtyRects\":"; s.dirtyRects.json(std::cout); std::cout << ",\"dirtyAreaPercent\":"; s.dirtyAreaPercent.json(std::cout);
    std::cout << ",\"framesPerTick\":"; s.framesPerTick.json(std::cout); std::cout << '}' << std::endl;
  };
  const auto paint = [&](const RECT* area) { RedrawWindow(window.load(), area, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); };
  // Warm-up: the first frames of a capture.
  { Stats warm; for (int i = 0; i < 5; ++i) { paint(nullptr); consume(warm, false, "warm-up"); } }
  // Partial: the box moves; every tenth trial stalls (five moves 12 ms apart
  // without consumption: the two-buffer pool drops frames).
  Stats partial;
  for (int trial = 1; trial <= trials; ++trial) {
    const bool stall = trial % 10 == 0;
    for (int step = 0; step < (stall ? 5 : 1); ++step) {
      RECT before, after; { std::lock_guard<std::mutex> guard(content.lock); before = content.box; after = content.boxAt(trial * 5 + step); content.box = after; }
      RECT both; UnionRect(&both, &before, &after); paint(&both);
      if (stall) Sleep(12);
    }
    consume(partial, stall, "partial-box");
    Sleep(8);
  }
  report("partial-box", partial);
  // Typing: the text row changes.
  Stats typing;
  for (int trial = 1; trial <= trials; ++trial) {
    RECT row; { std::lock_guard<std::mutex> guard(content.lock); content.rowSeed = uint32_t(trial) * 2654435761u; row = content.textRow(); }
    paint(&row); consume(typing, false, "typing-row"); Sleep(8);
  }
  report("typing-row", typing);
  // Full: everything repainted with another tint.
  Stats fullScene; const int fullTrials = std::max(10, trials / 4);
  for (int trial = 1; trial <= fullTrials; ++trial) {
    { std::lock_guard<std::mutex> guard(content.lock); content.tint = (trial * 13) % 60; content.paintDocument(); }
    paint(nullptr); consume(fullScene, false, "full-repaint"); Sleep(8);
  }
  report("full-repaint", fullScene);
  pool.FrameArrived(arrived); session.Close(); pool.Close(); CloseHandle(signal);
  PostMessageW(window.load(), WM_CLOSE, 0, 0); PostThreadMessageW(GetThreadId(ui.native_handle()), WM_QUIT, 0, 0); ui.join();
  return 0;
} catch (const winrt::hresult_error& error) { std::cerr << "WinRT 0x" << std::hex << uint32_t(error.code()) << '\n'; return 1; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
