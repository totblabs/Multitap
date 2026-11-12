// gpu-capture-v1 (gpu_capture.hpp). On a D3D11 device of this PC (WARP when it
// has no GPU): the GPU NV12 conversion and cursor composition against the CPU
// path's own bytes (convertBlockToNv12, Nv12CursorPatch), the per-tick GPU path
// decisions, the encode policy across GPU and CPU pictures, the encoder
// facade's texture input and its CPU fallback with injected backends and the
// real x264 + client decoder, and the NVENC registration cache. Synthetic
// textures only: no desktop capture, display, cursor or input is touched.
//   --no-restore      the composer keeps the previous cursor (fail-without-fix)
//   --keep-snapshot   the policy keeps its byte snapshot (fail-without-fix)
//   --benchmark [w h frames]  CPU readback path vs GPU path per frame
#include "gpu_capture.hpp"
#include "gpu_h264.hpp"
#include "gpu_h264_nvenc_policy.hpp"
#include "frame_encode_policy.hpp"
#include "lossless_tile_source.hpp"
#include "remote_decoder.h"
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>

namespace host {
#ifdef MULTITAP_X264
std::unique_ptr<EncoderImplementation> createX264Encoder(int, int, int, int, const X264Settings&);
#endif
struct EncoderGpuTestAccess {
  static void install(Encoder& encoder, std::unique_ptr<EncoderImplementation> implementation) {
    encoder.impl_ = std::move(implementation); encoder.backend_ = EncoderBackend::nvenc; encoder.gpuFallback_ = encoderGpuNone;
  }
};
}
namespace {
using namespace host;
using namespace host::gpucapture;
using capture::PixelRect;
int checks = 0;
std::string failures;
void require(bool value, const std::string& reason) { ++checks; if (!value) { failures += reason + "\n"; std::cerr << "FAIL: " << reason << "\n"; } }
void hard(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

struct Device {
  Com<ID3D11Device> device; Com<ID3D11DeviceContext> context; std::string name;
  explicit Device(bool hardware) {
    const HRESULT hr = D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    check(hr, "Test D3D11 device");
    Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
      std::wstring wide(desc.Description); name.clear(); for (wchar_t c : wide) name += (c >= 32 && c < 127) ? char(c) : '?';
    }
  }
  Com<ID3D11Texture2D> surface(int w, int h) const {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = UINT(w); desc.Height = UINT(h); desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> texture; check(device->CreateTexture2D(&desc, nullptr, &texture), "Test surface texture"); return texture;
  }
  // Uploads `rect` of the frame-sized BGRA buffer into `texture` (WGC's role).
  void upload(ID3D11Texture2D* texture, const std::vector<uint8_t>& frame, int frameWidth, const PixelRect& rect) const {
    context->UpdateSubresource(texture, 0, nullptr, frame.data() + (size_t(rect.top) * frameWidth + rect.left) * 4, UINT(frameWidth) * 4, 0);
  }
};

std::vector<uint8_t> randomFrame(int w, int h, uint32_t seed) {
  std::vector<uint8_t> frame(size_t(w) * h * 4); std::mt19937 random(seed);
  for (size_t i = 0; i < frame.size(); i += 4) {
    const uint32_t value = random();
    frame[i] = uint8_t(value); frame[i + 1] = uint8_t(value >> 8); frame[i + 2] = uint8_t(value >> 16); frame[i + 3] = 255;
  }
  // Extremes in the first rows: black, white, saturated primaries.
  const uint8_t extremes[][3] = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255}};
  for (int y = 0; y < std::min(h, 4); ++y) for (int x = 0; x < w; ++x) { const auto& e = extremes[(x / 2 + y) % 8]; auto* p = frame.data() + (size_t(y) * w + x) * 4; p[0] = e[0]; p[1] = e[1]; p[2] = e[2]; }
  return frame;
}
// The CPU path's NV12 of `frame` over `surfaces` (Capture::Modern direct path).
void cpuConvert(std::vector<uint8_t>& nv12, const std::vector<uint8_t>& frame, int w, int h, const std::vector<PixelRect>& surfaces) {
  for (const auto& rect : surfaces) capture::convertBlockToNv12(nv12, w, h, frame.data() + (size_t(rect.top) * w + rect.left) * 4, size_t(w) * 4, rect);
}
struct Difference { int luma = 0, chroma = 0, outside = 0; size_t bytes = 0; };
// `inside`: pixels of these rects may differ (the cursor); `outside` is the largest difference elsewhere.
Difference compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h, const std::vector<PixelRect>& inside = {}) {
  Difference result;
  const auto within = [&](int x, int y) { return std::any_of(inside.begin(), inside.end(), [&](const PixelRect& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }); };
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    const size_t at = size_t(y) * w + x; const int d = std::abs(int(a[at]) - int(b[at]));
    if (d) { ++result.bytes; result.luma = std::max(result.luma, d); if (!within(x, y)) result.outside = std::max(result.outside, d); }
  }
  for (int y = 0; y < h / 2; ++y) for (int x = 0; x < w; ++x) {
    const size_t at = size_t(w) * h + size_t(y) * w + x; const int d = std::abs(int(a[at]) - int(b[at]));
    if (d) { ++result.bytes; result.chroma = std::max(result.chroma, d); if (!within(x & ~1, y * 2)) result.outside = std::max(result.outside, d); }
  }
  return result;
}

// ------------------------------------------------------------------ conversion
std::string conversion(const Device& d) {
  std::ostringstream json; json << '[';
  struct Case { int w, h; std::vector<PixelRect> surfaces; };
  const std::vector<Case> cases{
    {1920, 1080, {{0, 0, 1920, 1080}}},
    {642, 362, {{0, 0, 642, 362}}},                                  // widths that are no multiple of 4/8/16 (CPU tails)
    {1280, 720, {{0, 0, 640, 720}, {640, 0, 1280, 720}}},            // two outputs side by side
    {800, 1000, {{0, 0, 800, 600}, {0, 600, 800, 1000}}},            // two outputs stacked
  };
  bool first = true;
  for (const auto& c : cases) {
    const auto frame = randomFrame(c.w, c.h, uint32_t(c.w * 31 + c.h));
    std::vector<Com<ID3D11Texture2D>> textures;
    FrameComposer composer(d.device.Get(), c.w, c.h, c.surfaces);
    for (size_t i = 0; i < c.surfaces.size(); ++i) { textures.push_back(d.surface(c.surfaces[i].width(), c.surfaces[i].height())); d.upload(textures[i].Get(), frame, c.w, c.surfaces[i]); composer.surfaceFrame(i, textures[i].Get()); }
    require(composer.compose(nullptr), "First compose did not report a change");
    std::vector<uint8_t> gpu(size_t(c.w) * c.h * 3 / 2), cpu(gpu.size());
    composer.read(gpu); cpuConvert(cpu, frame, c.w, c.h, c.surfaces);
    const auto diff = compare(gpu, cpu, c.w, c.h);
    require(diff.bytes == 0, "GPU NV12 differs from the CPU conversion at " + std::to_string(c.w) + "x" + std::to_string(c.h));
    require(!composer.compose(nullptr), "Compose without a new frame reported a change");
    // Only the last surface gets a new frame: the picture is still all current.
    auto next = randomFrame(c.w, c.h, uint32_t(c.w * 7 + 3));
    const auto& last = c.surfaces.back();
    auto mixed = frame;
    for (int y = last.top; y < last.bottom; ++y) std::memcpy(mixed.data() + (size_t(y) * c.w + last.left) * 4, next.data() + (size_t(y) * c.w + last.left) * 4, size_t(last.width()) * 4);
    d.upload(textures.back().Get(), mixed, c.w, last); composer.surfaceFrame(c.surfaces.size() - 1, textures.back().Get());
    require(composer.compose(nullptr), "A new surface frame was not reported as a change");
    composer.read(gpu); cpuConvert(cpu, mixed, c.w, c.h, c.surfaces);
    const auto partial = compare(gpu, cpu, c.w, c.h);
    require(partial.bytes == 0, "Partial surface update differs from the CPU conversion");
    if (!first) json << ','; first = false;
    json << "{\"size\":\"" << c.w << 'x' << c.h << "\",\"surfaces\":" << c.surfaces.size() << ",\"differentBytes\":" << diff.bytes + partial.bytes << '}';
  }
  json << ']'; return json.str();
}

// ------------------------------------------------------------------ cursors
struct Cursor { std::string name; HCURSOR handle; bool owned; bool representable; };
HCURSOR monochromeCursor(int size) {
  // AND/XOR planes: transparent (1,0), black (0,0), white (0,1) and inverting (1,1) quadrants.
  const int bytes = size / 8; std::vector<uint8_t> andPlane(size_t(bytes) * size), xorPlane(andPlane.size());
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
    const int quadrant = (y >= size / 2 ? 2 : 0) + (x >= size / 2 ? 1 : 0);
    const bool a = quadrant == 0 || quadrant == 3, b = quadrant == 2 || quadrant == 3;
    if (a) andPlane[size_t(y) * bytes + x / 8] |= uint8_t(0x80 >> (x % 8));
    if (b) xorPlane[size_t(y) * bytes + x / 8] |= uint8_t(0x80 >> (x % 8));
  }
  return CreateCursor(GetModuleHandleW(nullptr), 3, 2, size, size, andPlane.data(), xorPlane.data());
}
// A 32-bpp colour cursor: `alpha` true gives a per-pixel alpha ramp (blending);
// false leaves alpha 0 with the mask transparent and colours XORed (not affine).
HCURSOR colourCursor(int size, bool alpha) {
  BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = size; header.bV5Height = -size; header.bV5Planes = 1; header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS; header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00; header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
  HDC screen = GetDC(nullptr); void* bits = nullptr;
  HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!colour || !bits) return nullptr;
  auto* pixels = static_cast<uint8_t*>(bits);
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
    auto* p = pixels + (size_t(y) * size + x) * 4;
    if (alpha) { const uint8_t a = uint8_t(std::min(255, (x + y) * 255 / (2 * size - 2))); p[0] = uint8_t(40 + x * 5); p[1] = uint8_t(200 - y * 3); p[2] = 230; p[3] = a; }
    else { p[0] = 0x40; p[1] = 0x80; p[2] = 0xc0; p[3] = 0; }
  }
  std::vector<uint8_t> maskBits(size_t(size / 8) * size, alpha ? 0x00 : 0xff);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
  ICONINFO info{}; info.fIcon = FALSE; info.xHotspot = 1; info.yHotspot = 1; info.hbmMask = mask; info.hbmColor = colour;
  HCURSOR cursor = CreateIconIndirect(&info);
  DeleteObject(mask); DeleteObject(colour);
  return cursor;
}
capture::CursorDraw drawAt(HCURSOR cursor, int x, int y) {
  ICONINFO icon{}; int hx = 0, hy = 0;
  if (GetIconInfo(cursor, &icon)) { hx = int(icon.xHotspot); hy = int(icon.yHotspot); if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor); }
  return {cursor, x - hx, y - hy, GetSystemMetrics(SM_CXCURSOR), GetSystemMetrics(SM_CYCURSOR)};
}

std::string cursors(const Device& d) {
  constexpr int w = 1280, h = 720;
  const std::vector<PixelRect> surfaces{{0, 0, 640, 720}, {640, 0, 1280, 720}};
  HDC screen = GetDC(nullptr); hard(screen != nullptr, "Screen DC");
  std::vector<Cursor> list{
    {"arrow", LoadCursorW(nullptr, IDC_ARROW), false, true}, {"ibeam", LoadCursorW(nullptr, IDC_IBEAM), false, true},
    {"hand", LoadCursorW(nullptr, IDC_HAND), false, true}, {"sizeall", LoadCursorW(nullptr, IDC_SIZEALL), false, true},
    {"wait", LoadCursorW(nullptr, IDC_WAIT), false, true}, {"cross", LoadCursorW(nullptr, IDC_CROSS), false, true},
    {"monochrome4", monochromeCursor(32), true, true}, {"alphaRamp", colourCursor(32, true), true, true},
    {"xorColour", colourCursor(32, false), true, false},
  };
  std::ostringstream json; json << '['; bool first = true;
  int worstLuma = 0, worstChroma = 0;
  for (const auto& cursor : list) {
    hard(cursor.handle != nullptr, "Test cursor");
    auto frame = randomFrame(w, h, 99);
    std::vector<Com<ID3D11Texture2D>> textures;
    FrameComposer composer(d.device.Get(), w, h, surfaces);
    for (size_t i = 0; i < surfaces.size(); ++i) { textures.push_back(d.surface(surfaces[i].width(), surfaces[i].height())); d.upload(textures[i].Get(), frame, w, surfaces[i]); composer.surfaceFrame(i, textures[i].Get()); }
    capture::Nv12CursorPatch reference; hard(reference.reserve(std::max(64, GetSystemMetrics(SM_CXCURSOR) + 2), std::max(64, GetSystemMetrics(SM_CYCURSOR) + 2), screen), "Reference canvas");
    std::vector<uint8_t> cpu(size_t(w) * h * 3 / 2), gpu(cpu.size());
    cpuConvert(cpu, frame, w, h, surfaces);
    CursorOverlayRenderer renderer;
    const auto visit = [&](const PixelRect& rect, auto&& f) {
      for (const auto& surface : surfaces) if (const auto cut = capture::intersect(rect, surface)) f(frame.data() + (size_t(cut->top) * w + cut->left) * 4, size_t(w) * 4, *cut);
    };
    struct Step { std::optional<POINT> at; bool newBase; bool expectChange; };
    // Inside one output, across both (odd x), a new base under it, hidden,
    // clipped at a corner, the far edge, and an unchanged repeat.
    const std::vector<Step> steps{{POINT{100, 100}, false, true}, {POINT{637, 301}, false, true}, {POINT{637, 301}, true, true}, {std::nullopt, false, true},
                                  {POINT{2, 718}, false, true}, {POINT{1277, 3}, false, true}, {POINT{1277, 3}, false, false}};
    int maxLuma = 0, maxChroma = 0, maxOutside = 0; bool unrepresentable = false; size_t index = 0, drawnBytes = 0;
    for (const auto& step : steps) {
      ++index;
      if (step.newBase) {
        const auto next = randomFrame(w, h, uint32_t(500 + index));
        const auto& right = surfaces[1];
        for (int y = right.top; y < right.bottom; ++y) std::memcpy(frame.data() + (size_t(y) * w + right.left) * 4, next.data() + (size_t(y) * w + right.left) * 4, size_t(right.width()) * 4);
        d.upload(textures[1].Get(), frame, w, right); composer.surfaceFrame(1, textures[1].Get());
        capture::convertBlockToNv12(cpu, w, h, frame.data() + size_t(right.left) * 4, size_t(w) * 4, right);
      }
      std::optional<capture::CursorDraw> draw; if (step.at) draw = drawAt(cursor.handle, step.at->x, step.at->y);
      // CPU reference: exactly the direct capture path's patch.
      (void)reference.apply(cpu, w, h, draw, visit, screen);
      const CursorOverlay* overlay = nullptr;
      try { if (draw) overlay = renderer.render(*draw, w, h, screen); }
      catch (const CursorUnrepresentable&) { unrepresentable = true; break; }
      const bool changed = composer.compose(overlay);
      require(changed == step.expectChange, cursor.name + " step " + std::to_string(index) + ": compose change flag");
      composer.read(gpu);
      std::vector<PixelRect> patches; if (overlay) patches.push_back(overlay->patch);
      if (draw) {
        // The cursor really changed pixels (a failed DrawIconEx on both sides would compare equal).
        std::vector<uint8_t> bare(cpu.size()); cpuConvert(bare, frame, w, h, surfaces);
        const auto drawn = compare(cpu, bare, w, h);
        require(drawn.bytes > 0, cursor.name + " step " + std::to_string(index) + ": the reference drew no cursor");
        drawnBytes += drawn.bytes;
      }
      const auto diff = compare(gpu, cpu, w, h, patches);
      maxLuma = std::max(maxLuma, diff.luma); maxChroma = std::max(maxChroma, diff.chroma); maxOutside = std::max(maxOutside, diff.outside);
      require(diff.outside == 0, cursor.name + " step " + std::to_string(index) + ": GPU picture differs outside the cursor patch (stale cursor?)");
    }
    if (cursor.representable) {
      require(!unrepresentable, cursor.name + ": an affine cursor was refused");
      require(maxLuma <= 1 && maxChroma <= 1, cursor.name + ": cursor pixels differ by more than 1 from the CPU patch");
      worstLuma = std::max(worstLuma, maxLuma); worstChroma = std::max(worstChroma, maxChroma);
    } else require(unrepresentable, cursor.name + ": an XOR-colour cursor was not detected");
    if (!first) json << ','; first = false;
    json << "{\"cursor\":\"" << cursor.name << "\",\"gpu\":" << (unrepresentable ? "false" : "true") << ",\"cursorBytes\":" << drawnBytes << ",\"maxLuma\":" << maxLuma << ",\"maxChroma\":" << maxChroma << ",\"maxOutside\":" << maxOutside << '}';
    if (cursor.owned) DestroyCursor(cursor.handle);
  }
  ReleaseDC(nullptr, screen);
  json << "],\"worstLuma\":" << worstLuma << ",\"worstChroma\":" << worstChroma;
  return json.str();
}

// ------------------------------------------------------------------ affine model
void affine() {
  bool exact = true;
  for (int base = 0; base < 256; ++base) {
    exact = exact && cursorAffine(0, 255, base) == base && cursorAffine(0, 0, base) == 0 && cursorAffine(255, 255, base) == 255 && cursorAffine(255, 0, base) == 255 - base;
  }
  require(exact, "Affine model is not exact for transparent, black, white and inverting pixels");
  // XOR with a colour: grey checks catch it.
  const uint8_t black[4]{0x40, 0x80, 0xc0, 0}, white[4]{uint8_t(255 ^ 0x40), uint8_t(255 ^ 0x80), uint8_t(255 ^ 0xc0), 0};
  const uint8_t grey1[4]{uint8_t(0x55 ^ 0x40), uint8_t(0x55 ^ 0x80), uint8_t(0x55 ^ 0xc0), 0}, grey2[4]{uint8_t(0xaa ^ 0x40), uint8_t(0xaa ^ 0x80), uint8_t(0xaa ^ 0xc0), 0};
  require(!cursorCanvasesAffine({black, 4}, {white, 4}, {grey1, 4}, 0x55, {grey2, 4}, 0xaa, 1, 1), "XOR-colour pixel accepted as affine");
  const uint8_t tb[4]{0, 0, 0, 0}, tw[4]{255, 255, 255, 0}, t1[4]{0x55, 0x55, 0x55, 0}, t2[4]{0xaa, 0xaa, 0xaa, 0};
  require(cursorCanvasesAffine({tb, 4}, {tw, 4}, {t1, 4}, 0x55, {t2, 4}, 0xaa, 1, 1), "Transparent pixel refused");
}

// ------------------------------------------------------------------ decisions
void decisions() {
  GpuArmInput in{true, true, true, true, true};
  require(gpuArmDecision(in) == gpuPathActive, "Arm: everything in place");
  in.hardwareEncoder = false; require(!gpuArmDecision(in), "Arm: a CPU encoder must arm and record nothing");
  in = {true, false, true, true, true}; require(gpuArmDecision(in) == gpuPathBackendUnsupported, "Arm: AMF/QSV");
  in = {true, true, false, true, true}; require(gpuArmDecision(in) == gpuPathNoHardwareCapture, "Arm: WARP/GDI capture");
  in = {true, true, true, false, true}; require(gpuArmDecision(in) == gpuPathLayout, "Arm: scaled capture");
  in = {true, true, true, true, false}; require(gpuArmDecision(in) == gpuPathOtherDevice, "Arm: encoder on another device");
  require(gpuPathRecord(gpuPathConverter, GpuStep::formatSupport, HRESULT(0x80004001)) == (5u | (3u << 8) | (0x4001u << 16)), "Record packing");

  std::vector<uint32_t> records; GpuPathSession session([&](uint32_t value) { records.push_back(value); });
  session.armed(std::nullopt);
  require(records.empty() && !session.tick(false, true), "CPU encoder: no record, no GPU tick");
  session.armed(gpuPathRecord(gpuPathActive));
  require(records == std::vector<uint32_t>{1} && session.tick(false, true), "Armed: GPU tick");
  require(!session.tick(true, true) && session.active(), "Tiles wanted: CPU tick, path kept");
  require(session.tick(false, true), "Tiles paused (retired): GPU tick again");
  session.armed(gpuPathRecord(gpuPathActive));
  require(records.size() == 1, "A repeated record is suppressed");
  session.armed(gpuPathRecord(gpuPathOtherDevice));
  require(records.back() == gpuPathRecord(gpuPathDeviceChanged) && !session.tick(false, true), "Rebuild lost the device: recorded as device changed");
  session.armed(gpuPathRecord(gpuPathActive));
  require(!session.tick(false, false) && records.back() == gpuPathRecord(gpuPathEncoderLeft) && !session.active(), "Encoder fallback ends the GPU path");
  require(!session.tick(false, true), "No GPU tick after the encoder left");
  session.armed(gpuPathRecord(gpuPathActive)); session.failed(gpuPathRecord(gpuPathConvertFailed, GpuStep::convert, E_FAIL));
  require(gpuPathRecordReason(records.back()) == gpuPathConvertFailed && !session.tick(false, true), "Capture failure ends the GPU path");
}

// ------------------------------------------------------------------ encode policy
void policy() {
  constexpr size_t bytes = 64 * 48 * 3 / 2;
  FrameEncodePolicy policy(bytes);
  const std::vector<uint8_t> a(bytes, 10);
  uint64_t now = 1000;
  const auto out = [&](const FrameEncodeDecision& d, bool key = false) { if (d.submit) require(policy.output(now, key, now + 1), "Policy output"); now += 1000; };
  auto d = policy.prepare(a, now); require(d.submit && d.contentChanged, "First CPU frame submits"); out(d, true);
  d = policy.prepareChanged(true, now); require(d.submit && d.contentChanged, "Changed GPU picture submits"); out(d);
  d = policy.prepareChanged(false, now); require(!d.submit, "Unchanged GPU picture does not submit");
  d = policy.prepare(a, now);
  require(d.submit && d.contentChanged, "CPU frame equal to the pre-GPU snapshot must count as changed after a GPU picture");
  out(d);
  d = policy.prepare(a, now); require(!d.submit, "Unchanged CPU frame does not submit");
  d = policy.prepareChanged(false, now, true); require(d.submit && d.forced, "Forced GPU tick submits (tile drop / IDR request)"); out(d);
  now += FrameEncodePolicy::refreshIntervalUs;
  d = policy.prepareChanged(false, now); require(d.submit && d.refresh, "GPU path keeps the 1 s refresh"); out(d);
  // Refinement on GPU pictures: a large change output arms it, quiet ticks refine.
  FrameEncodePolicy refining(bytes, FrameRefinement::forCbr(8000000, 60));
  uint64_t t = 1000;
  auto r = refining.prepareChanged(true, t); require(r.submit, "Refinement policy first picture"); require(refining.output(t, true, t + 1, 1), "IDR output"); t += 16667;
  r = refining.prepareChanged(true, t); require(r.submit, "Large change"); require(refining.output(t, false, t + 1, 100000), "Large output"); t += 200000;
  r = refining.prepareChanged(false, t); require(r.submit && r.refine, "Quiet GPU picture refines after a large change");
}

// ------------------------------------------------------------------ lossless retirement on a GPU tick
void retirement() {
  LosslessTileSource source(64, 48, LosslessSettings::forBitrate(4000000));
  FrameEncodePolicy policy(64 * 48 * 3 / 2);
  bool retired = false; int encodes = 0; uint64_t now = 1000;
  const auto prepare = [&](bool forced) { return policy.prepareChanged(false, now, forced); };
  // First a picture so an IDR exists.
  auto first = policy.prepareChanged(true, now); require(first.submit, "Retirement fixture first picture"); require(policy.output(now, true, now + 1), "Retirement fixture IDR"); now += 1000;
  auto tick = losslessRetireTick(source, retired, prepare, now, [&] { ++encodes; }, false);
  require(tick && tick->payload && tick->decision.submit && retired && encodes == 1, "GPU tick retires tiles with one video frame");
  require(policy.output(now, false, now + 1), "Retirement output"); now += 1000;
  tick = losslessRetireTick(source, retired, prepare, now, [&] { ++encodes; }, false);
  require(!tick && encodes == 1, "Retired tiles: GPU tick encodes as without tiles");
  tick = losslessRetireTick(source, retired, prepare, now, [&] { ++encodes; }, true);
  require(tick && tick->payload && encodes == 2, "Forced GPU tick retires again (IDR request)");
}

// ------------------------------------------------------------------ NVENC registrations
void registrations() {
  gpu::NvencRegistrationCache<int, int, 2> cache; std::vector<int> registered, released;
  bool refuse = false;
  const auto reg = [&](int key) { if (refuse) throw std::runtime_error("register"); registered.push_back(key); return key * 10; };
  const auto unreg = [&](int handle) { if (refuse) throw std::runtime_error("unregister"); released.push_back(handle); };
  require(cache.acquire(1, reg, unreg) == 10 && cache.acquire(1, reg, unreg) == 10 && registered == std::vector<int>{1}, "Registration reused");
  require(cache.acquire(2, reg, unreg) == 20 && cache.live() == 2, "Second registration");
  (void)cache.acquire(1, reg, unreg);
  require(cache.acquire(3, reg, unreg) == 30 && released == std::vector<int>{20} && cache.live() == 2, "Least recently used evicted first");
  refuse = true; bool threw = false;
  try { (void)cache.acquire(4, reg, unreg); } catch (const std::exception&) { threw = true; }
  require(threw && cache.live() == 2 && released.size() == 1, "A refused release leaves the cache unchanged");
  refuse = false;
  require(cache.acquire(3, reg, unreg) == 30 && cache.acquire(1, reg, unreg) == 10, "Entries kept after a refused release");
}

// ------------------------------------------------------------------ encoder facade
#ifdef MULTITAP_X264
constexpr int ew = 320, eh = 180, efps = 60, ebitrate = 4000000;
// "Takes" the texture of its device like NVENC does (its own readback stands in
// for the encoder reading GPU memory), counted; fails at `failAt`.
class InjectedTexture final : public EncoderImplementation {
 public:
  InjectedTexture(ID3D11Device* device, gpu::Adapter adapter, int failAt) : device_(device), cpu_(createX264Encoder(ew, eh, efps, ebitrate, {})), adapter_(std::move(adapter)), failAt_(failAt) {}
  void encode(const uint8_t* nv12, uint64_t pts, std::vector<Frame>& frames) override { ++bytes; cpu_->encode(nv12, pts, frames); verified_ = true; }
  void encodePicture(const EncoderPicture& picture, uint64_t pts, std::vector<Frame>& frames) override {
    if (!picture.texture) { EncoderImplementation::encodePicture(picture, pts, frames); return; }
    Com<ID3D11Device> owner; picture.texture->GetDevice(&owner);
    if (owner.Get() != device_) { EncoderImplementation::encodePicture(picture, pts, frames); return; }
    if (calls_++ == failAt_) throw gpu::Failure(encoderGpuSubmissionFailed, "injected texture map failure", gpu::failureDetail(EncoderGpuFailureSite::textureMap, 7));
    ++textures;
    D3D11_TEXTURE2D_DESC desc{}; picture.texture->GetDesc(&desc); desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    Com<ID3D11Texture2D> staging; check(device_->CreateTexture2D(&desc, nullptr, &staging), "Injected staging");
    Com<ID3D11DeviceContext> context; device_->GetImmediateContext(&context); context->CopyResource(staging.Get(), picture.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Injected map");
    std::vector<uint8_t> nv12(size_t(ew) * eh * 3 / 2);
    for (int y = 0; y < eh; ++y) std::memcpy(nv12.data() + size_t(y) * ew, static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, ew);
    for (int y = 0; y < eh / 2; ++y) std::memcpy(nv12.data() + size_t(ew) * eh + size_t(y) * ew, static_cast<uint8_t*>(mapped.pData) + size_t(eh + y) * mapped.RowPitch, ew);
    context->Unmap(staging.Get(), 0);
    cpu_->encode(nv12.data(), pts, frames); verified_ = true;
  }
  ID3D11Device* textureDevice() const override { return device_; }
  void finish(std::vector<Frame>& frames) override { cpu_->finish(frames); }
  bool setBitrate(int value) override { return cpu_->setBitrate(value); }
  int reportedBitrate() const override { return cpu_->reportedBitrate(); }
  EncoderTuning reportedTuning() const override { return {}; }
  void requestKeyFrame() override { cpu_->requestKeyFrame(); }
  EncoderHardwareInfo hardwareInfo() const override {
    EncoderHardwareInfo info; info.initialized = true; info.hardwareVerified = verified_; info.adapterLuid = adapter_.luid; info.vendorId = adapter_.vendorId; info.deviceId = adapter_.deviceId; return info;
  }
  int textures = 0, bytes = 0;
 private:
  ID3D11Device* device_; std::unique_ptr<EncoderImplementation> cpu_; gpu::Adapter adapter_; int failAt_, calls_ = 0; bool verified_ = false;
};
std::vector<uint8_t> moving(int frame) {
  std::vector<uint8_t> bgra(size_t(ew) * eh * 4);
  for (int y = 0; y < eh; ++y) for (int x = 0; x < ew; ++x) { auto* p = bgra.data() + (size_t(y) * ew + x) * 4; p[0] = uint8_t(x * 2 + frame * 9); p[1] = uint8_t(y * 3 + frame * 5); p[2] = uint8_t((x / 16 + y / 16 + frame) % 2 ? 220 : 40); p[3] = 255; }
  return bgra;
}
std::string encoderFacade(const Device& d) {
  Runtime runtime;
  const std::vector<PixelRect> one{{0, 0, ew, eh}};
  FrameComposer composer(d.device.Get(), ew, eh, one);
  auto texture = d.surface(ew, eh);
  std::vector<uint8_t> readback(size_t(ew) * eh * 3 / 2); int reads = 0;
  const auto picture = [&] { return EncoderPicture{nullptr, composer.picture(), [&]() -> const uint8_t* { ++reads; composer.read(readback); return readback.data(); }}; };
  Encoder encoder(ew, eh, efps, ebitrate, {}, EncoderBackend::x264);
  auto injected = std::make_unique<InjectedTexture>(d.device.Get(), gpu::Adapter{77, 0x10de, 1, "Injected texture GPU"}, 3);
  auto* raw = injected.get();
  EncoderGpuTestAccess::install(encoder, std::move(injected));
  require(encoder.textureDevice() == d.device.Get(), "Injected backend takes this device's textures");
  multitap::RemoteDecoder decoder; check(decoder.Initialize(ew, eh), "Client decoder");
  std::vector<uint64_t> encoded, decoded, keys; std::vector<int> readsAt;
  const auto shown = [&](multitap::DecodedFrame&& frame) { decoded.push_back(frame.pts_us); };
  const auto output = [&](Frame frame) { if (frame.key) keys.push_back(frame.ptsUs); encoded.push_back(frame.ptsUs); check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, shown), "Decode across the texture fallback"); };
  int texturesBeforeFallback = 0;
  for (int n = 0; n < 8; ++n) {
    const auto bgra = moving(n); d.upload(texture.Get(), bgra, ew, one[0]); composer.surfaceFrame(0, texture.Get()); (void)composer.compose(nullptr);
    const uint64_t pts = uint64_t(n) * 1000000 / efps; const int before = reads;
    encoder.encode(picture(), pts, output);
    readsAt.push_back(reads - before);
    if (n == 2) texturesBeforeFallback = raw->textures;
    require(encoded.size() == size_t(n + 1) && encoded.back() == pts, "Every picture yields exactly its own output (pts " + std::to_string(pts) + ")");
  }
  encoder.finish(output); check(decoder.Drain(shown), "Drain client decoder");
  require(texturesBeforeFallback == 3, "Texture path used without any readback before the failure");
  require(readsAt[0] == 0 && readsAt[1] == 0 && readsAt[2] == 0, "No readback while the backend takes textures");
  require(readsAt[3] == 1, "The failed picture is read back once for the CPU encoder");
  require(encoder.backend() == EncoderBackend::x264 && encoder.textureDevice() == nullptr, "After the failure: CPU backend, no texture device");
  require(encoder.hardwareInfo().gpuFallbackCode == encoderGpuSubmissionFailed && encoder.hardwareInfo().gpuFailure.site == uint32_t(EncoderGpuFailureSite::textureMap), "Fixed GPU fallback code and texture site kept");
  require(keys.size() >= 2 && keys[0] == 0 && keys[1] == uint64_t(3) * 1000000 / efps, "Fallback IDR exactly at the failed picture");
  require(decoded == encoded && decoded.size() == 8, "Client decoder shows every picture across the fallback");

  // Bounded candidates: a first candidate taking textures fails its first
  // picture; the next one (another adapter) encodes the read-back bytes.
  const std::vector<gpu::Adapter> adapters{{1, 0x10de, 1, "capture GPU"}, {2, 0x10de, 2, "other GPU"}};
  int opened = 0; InjectedTexture* second = nullptr;
  auto candidates = gpu::createVerifiedFromAdapters(adapters, {gpu::Mode::automatic, {}}, {1, {}}, [&](const gpu::Adapter& adapter) -> std::unique_ptr<EncoderImplementation> {
    ++opened;
    if (adapter.luid == 1) return std::make_unique<InjectedTexture>(d.device.Get(), adapter, 0);
    auto other = std::make_unique<InjectedTexture>(nullptr, adapter, -1); second = other.get(); return other;
  });
  require(candidates->textureDevice() == d.device.Get(), "First candidate is on the capture's device");
  reads = 0; std::vector<Frame> frames;
  candidates->encodePicture(picture(), 0, frames);
  require(opened == 2 && frames.size() == 1 && reads == 1 && second && second->bytes == 1, "The next candidate encoded the read-back picture");
  require(candidates->textureDevice() == nullptr, "The running candidate takes no textures");
  std::ostringstream json; json << "{\"textureEncodes\":" << texturesBeforeFallback << ",\"readsPerTick\":[";
  for (size_t i = 0; i < readsAt.size(); ++i) json << (i ? "," : "") << readsAt[i];
  json << "],\"decoded\":" << decoded.size() << '}';
  return json.str();
}
#endif

// ------------------------------------------------------------------ benchmark
double cyclesPerMs() {
  static const double rate = [] {
    LARGE_INTEGER f{}, a{}, b{}; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a); const auto c0 = __rdtsc(); Sleep(250); const auto c1 = __rdtsc(); QueryPerformanceCounter(&b);
    return double(c1 - c0) / (double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
  }();
  return rate;
}
uint64_t processCycles() { ULONG64 value = 0; QueryProcessCycleTime(GetCurrentProcess(), &value); return value; }
// The capture thread's own CPU work (WARP's worker threads, which stand in for the GPU, excluded).
uint64_t threadCycles() { ULONG64 value = 0; QueryThreadCycleTime(GetCurrentThread(), &value); return value; }
struct Series {
  std::vector<double> values;
  double mean() const { return values.empty() ? 0 : std::accumulate(values.begin(), values.end(), 0.0) / double(values.size()); }
  double at(double q) const { if (values.empty()) return 0; auto s = values; std::sort(s.begin(), s.end()); return s[std::min(s.size() - 1, size_t(q * double(s.size() - 1) + 0.5))]; }
  std::string json() const { std::ostringstream o; o << std::fixed << std::setprecision(3) << "{\"mean\":" << mean() << ",\"p50\":" << at(.5) << ",\"p95\":" << at(.95) << '}'; return o.str(); }
};
std::string benchmark(bool hardware, int w, int h, int frames) {
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::duration v) { return std::chrono::duration<double, std::milli>(v).count(); };
  Device d(hardware);
  const PixelRect whole{0, 0, w, h};
  auto texture = d.surface(w, h);
  D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc); desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Com<ID3D11Texture2D> staging; check(d.device->CreateTexture2D(&desc, nullptr, &staging), "Benchmark staging");
  D3D11_QUERY_DESC q{}; q.Query = D3D11_QUERY_EVENT; Com<ID3D11Query> query; check(d.device->CreateQuery(&q, &query), "Benchmark query");
  const auto finish = [&] { d.context->End(query.Get()); d.context->Flush(); BOOL done = FALSE; while (d.context->GetData(query.Get(), &done, sizeof(done), 0) != S_OK || !done) SwitchToThread(); };
  std::vector<std::vector<uint8_t>> scenes; for (int i = 0; i < 8; ++i) scenes.push_back(randomFrame(w, h, uint32_t(1000 + i)));
  std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2), encoderInput(nv12.size());
  FrameComposer composer(d.device.Get(), w, h, {whole});
  // Wall: elapsed (this Idle-priority process on a busy desktop: scheduling
  // included). threadCpu: the capture thread's own work. processCpu: every
  // thread, i.e. with WARP's emulation of the GPU work.
  Series cpuReadbackWall, cpuReadbackThread, cpuConvertThread, cpuWall, cpuThread, cpuProcess, cpuCopyToEncoderThread,
         gpuSubmitWall, gpuSubmitThread, gpuWall, gpuProcess, gpuReadbackWall;
  const double rate = cyclesPerMs();
  const auto cpu = [&](uint64_t from, uint64_t to) { return double(to - from) / rate; };
  for (int n = 0; n < frames + 5; ++n) {
    d.upload(texture.Get(), scenes[size_t(n) % scenes.size()], w, whole); finish();
    // CPU path (Capture::Modern direct): copy -> staging -> Map, convert, then
    // the bytes are copied once more into the encoder's input buffer.
    const auto p0 = processCycles(); const auto c0 = threadCycles(); const auto t0 = Clock::now();
    d.context->CopyResource(staging.Get(), texture.Get()); D3D11_MAPPED_SUBRESOURCE mapped{}; check(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Benchmark map");
    const auto t1 = Clock::now(); const auto c1 = threadCycles();
    capture::convertBlockToNv12(nv12, w, h, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, whole);
    d.context->Unmap(staging.Get(), 0);
    const auto t2 = Clock::now(); const auto c2 = threadCycles(); const auto p2 = processCycles();
    std::memcpy(encoderInput.data(), nv12.data(), nv12.size());
    const auto c3 = threadCycles();
    if (n >= 5) {
      cpuReadbackWall.values.push_back(ms(t1 - t0)); cpuReadbackThread.values.push_back(cpu(c0, c1)); cpuConvertThread.values.push_back(cpu(c1, c2));
      cpuWall.values.push_back(ms(t2 - t0)); cpuThread.values.push_back(cpu(c0, c3)); cpuProcess.values.push_back(cpu(p0, p2)); cpuCopyToEncoderThread.values.push_back(cpu(c2, c3));
    }
    d.upload(texture.Get(), scenes[size_t(n + 3) % scenes.size()], w, whole); finish();
    // GPU path: GPU copy + conversion submitted, then waited for (NVENC waits for it).
    const auto q0 = processCycles(); const auto g0 = threadCycles(); const auto s0 = Clock::now();
    composer.surfaceFrame(0, texture.Get()); (void)composer.compose(nullptr);
    const auto s1 = Clock::now(); const auto g1 = threadCycles(); finish(); const auto s2 = Clock::now(); const auto q2 = processCycles();
    if (n >= 5) { gpuSubmitWall.values.push_back(ms(s1 - s0)); gpuSubmitThread.values.push_back(cpu(g0, g1)); gpuWall.values.push_back(ms(s2 - s0)); gpuProcess.values.push_back(cpu(q0, q2)); }
    const auto r0 = Clock::now(); composer.read(nv12); const auto r1 = Clock::now();
    if (n >= 5) gpuReadbackWall.values.push_back(ms(r1 - r0));
  }
  std::ostringstream json;
  json << "{\"device\":\"" << d.name << "\",\"driver\":\"" << (hardware ? "hardware" : "warp") << "\",\"size\":\"" << w << 'x' << h << "\",\"frames\":" << frames
       << ",\"cpuPath\":{\"readbackWallMs\":" << cpuReadbackWall.json() << ",\"readbackThreadCpuMs\":" << cpuReadbackThread.json() << ",\"convertThreadCpuMs\":" << cpuConvertThread.json()
       << ",\"copyToEncoderInputThreadCpuMs\":" << cpuCopyToEncoderThread.json() << ",\"threadCpuMs\":" << cpuThread.json() << ",\"wallMs\":" << cpuWall.json() << ",\"processCpuMs\":" << cpuProcess.json() << '}'
       << ",\"gpuPath\":{\"submitThreadCpuMs\":" << gpuSubmitThread.json() << ",\"submitWallMs\":" << gpuSubmitWall.json() << ",\"wallToReadyMs\":" << gpuWall.json()
       << ",\"processCpuMs\":" << gpuProcess.json() << ",\"fallbackNv12ReadbackWallMs\":" << gpuReadbackWall.json() << "}}";
  return json.str();
}
}

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == "--benchmark") {
      const int w = args.size() > 2 ? std::stoi(args[1]) : 1920, h = args.size() > 2 ? std::stoi(args[2]) : 1080, frames = args.size() > 3 ? std::stoi(args[3]) : 120;
      std::cout << "{\"kind\":\"gpu-capture-benchmark\",\"warp\":" << benchmark(false, w, h, frames) << ",\"defaultHardwareDevice\":" << benchmark(true, w, h, frames)
                << ",\"realGpu\":false,\"nvenc\":false,\"note\":\"no GPU on this PC: both devices are software rasterizers\"}\n";
      return 0;
    }
    for (const auto& arg : args) {
      if (arg == "--no-restore") FrameComposer::restorePreviousCursor = false;
      else if (arg == "--keep-snapshot") FrameEncodePolicy::dropSnapshot = false;
      else throw std::runtime_error("Unknown option");
    }
    Device warp(false);
    affine();
    decisions();
    policy();
    retirement();
    registrations();
    const auto converted = conversion(warp);
    const auto cursorResults = cursors(warp);
    std::string encoder = "null";
#ifdef MULTITAP_X264
    encoder = encoderFacade(warp);
#endif
    const bool passed = failures.empty();
    std::cout << "{\"kind\":\"gpu-capture-test\",\"device\":\"" << warp.name << "\",\"checks\":" << checks << ",\"conversion\":" << converted
              << ",\"cursors\":" << cursorResults << ",\"encoder\":" << encoder << ",\"realGpu\":false,\"nvenc\":false,\"desktopCaptured\":false,\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::cout << "{\"kind\":\"gpu-capture-test\",\"passed\":false}\n"; std::cerr << error.what() << '\n'; return 1;
  }
}
