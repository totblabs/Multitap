#include "gpu_capture.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <mutex>

namespace host::gpucapture {
namespace {
template<class T> using Com = Microsoft::WRL::ComPtr<T>;
// The CPU path's integers (native/windows_video/bgra_nv12_sse2.hpp): luma per
// pixel, chroma from the floor average of each 2x2 block, BT.601 limited range.
// Every value is an exact integer: UNORM8 reads are n/255 (rounded back to n),
// writes of y/255 round to y. Only non-negative numbers are divided; `>>` on
// int is arithmetic like the CPU's srai.
constexpr char shaderSource[] = R"(
Texture2D<float4> baseTexture : register(t0);
Texture2D<float4> blackTexture : register(t1);
Texture2D<float4> whiteTexture : register(t2);
cbuffer Region : register(b0) { int2 baseOrigin; int2 overlayOrigin; int2 overlaySize; int overlayOn; int unused; };
float4 vertexMain(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
int3 bytes(float4 value) { return int3(round(saturate(value.rgb) * 255.0)); }
int3 floorDiv255(int3 n) { int3 p = max(n, 0) / 255; int3 m = (254 - min(n, 0)) / 255; return n >= 0 ? p : -m; }
int3 composed(int2 p) {
  int3 c = bytes(baseTexture.Load(int3(p - baseOrigin, 0)));
  if (overlayOn != 0) {
    int2 q = p - overlayOrigin;
    if (q.x >= 0 && q.y >= 0 && q.x < overlaySize.x && q.y < overlaySize.y) {
      int3 b = bytes(blackTexture.Load(int3(q, 0)));
      int3 w = bytes(whiteTexture.Load(int3(q, 0)));
      c = b + floorDiv255(c * (w - b) + 127);
    }
  }
  return clamp(c, 0, 255);
}
float lumaMain(float4 position : SV_Position) : SV_Target {
  int3 c = composed(int2(position.xy));
  return float(clamp(((66 * c.r + 129 * c.g + 25 * c.b + 128) >> 8) + 16, 0, 255)) / 255.0;
}
float2 chromaMain(float4 position : SV_Position) : SV_Target {
  int2 p = int2(position.xy) * 2;
  int3 a = (composed(p) + composed(p + int2(1, 0)) + composed(p + int2(0, 1)) + composed(p + int2(1, 1))) >> 2;
  int u = ((-38 * a.r - 74 * a.g + 112 * a.b + 128) >> 8) + 128;
  int v = ((112 * a.r - 94 * a.g - 18 * a.b + 128) >> 8) + 128;
  return float2(clamp(u, 0, 255), clamp(v, 0, 255)) / 255.0;
}
)";
struct Bytecode { std::vector<uint8_t> vertex, luma, chroma; };
// Compiled once per process with the system's d3dcompiler_47 (System32 only);
// the bytecode is copied out, so the compiler module may stay loaded unused.
const Bytecode& bytecode() {
  static std::mutex mutex; static std::optional<Bytecode> compiled;
  std::lock_guard lock(mutex);
  if (compiled) return *compiled;
  static HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) throw GpuFailure(GpuStep::compiler, HRESULT_FROM_WIN32(GetLastError()), "Shader compiler unavailable");
  const auto compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")));
  if (!compile) throw GpuFailure(GpuStep::compiler, HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "Shader compiler entry unavailable");
  const auto one = [&](const char* entry, const char* target) {
    Com<ID3DBlob> code, errors;
    const HRESULT hr = compile(shaderSource, sizeof(shaderSource) - 1, "gpu_capture", nullptr, nullptr, entry, target,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) || !code) throw GpuFailure(GpuStep::shaders, FAILED(hr) ? hr : E_FAIL, "Capture conversion shader did not compile");
    const auto* first = static_cast<const uint8_t*>(code->GetBufferPointer());
    return std::vector<uint8_t>(first, first + code->GetBufferSize());
  };
  Bytecode value;
  value.vertex = one("vertexMain", "vs_4_0"); value.luma = one("lumaMain", "ps_4_0"); value.chroma = one("chromaMain", "ps_4_0");
  compiled = std::move(value);
  return *compiled;
}
void require(HRESULT hr, GpuStep step, const char* what) { if (FAILED(hr)) throw GpuFailure(step, hr, what); }
struct Constants { int32_t baseX, baseY, overlayX, overlayY, overlayWidth, overlayHeight, overlayOn, unused; };
static_assert(sizeof(Constants) == 32);
}

struct FrameComposer::Impl {
  struct Surface { capture::PixelRect placed; Com<ID3D11Texture2D> latest; Com<ID3D11ShaderResourceView> view; bool valid = false, dirty = false; };
  Com<ID3D11Device> device; Com<ID3D11DeviceContext> context;
  int width = 0, height = 0;
  std::vector<Surface> surfaces;
  Com<ID3D11Texture2D> target, staging;
  Com<ID3D11RenderTargetView> luma, chroma;
  Com<ID3D11VertexShader> vertex; Com<ID3D11PixelShader> lumaShader, chromaShader;
  Com<ID3D11Buffer> constants; Com<ID3D11RasterizerState> rasterizer;
  Com<ID3D11Texture2D> black, white; Com<ID3D11ShaderResourceView> blackView, whiteView;
  int canvasWidth = 0, canvasHeight = 0;
  std::optional<CursorOverlay> previous;

  Com<ID3D11Texture2D> texture(int w, int h, DXGI_FORMAT format, UINT bind, GpuStep step) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = UINT(w); desc.Height = UINT(h); desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
    Com<ID3D11Texture2D> result; require(device->CreateTexture2D(&desc, nullptr, &result), step, "Capture GPU texture"); return result;
  }
  Com<ID3D11ShaderResourceView> view(ID3D11Texture2D* source, GpuStep step) {
    Com<ID3D11ShaderResourceView> result; require(device->CreateShaderResourceView(source, nullptr, &result), step, "Capture GPU texture view"); return result;
  }
  void canvases(int w, int h) {
    if (black && w <= canvasWidth && h <= canvasHeight) return;
    const int cw = std::max({w, canvasWidth, 64}), ch = std::max({h, canvasHeight, 64});
    blackView.Reset(); whiteView.Reset(); black.Reset(); white.Reset(); canvasWidth = canvasHeight = 0;
    black = texture(cw, ch, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, GpuStep::cursor);
    white = texture(cw, ch, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, GpuStep::cursor);
    blackView = view(black.Get(), GpuStep::cursor); whiteView = view(white.Get(), GpuStep::cursor);
    canvasWidth = cw; canvasHeight = ch;
  }
  // Converts `region` (even, inside surface `index`) into the picture, with
  // the uploaded cursor canvases composed over `overlay` when given.
  void convert(size_t index, const capture::PixelRect& region, const capture::PixelRect* overlay) {
    const auto& surface = surfaces[index];
    const Constants values{surface.placed.left, surface.placed.top, overlay ? overlay->left : 0, overlay ? overlay->top : 0,
                           overlay ? overlay->width() : 0, overlay ? overlay->height() : 0, overlay ? 1 : 0, 0};
    context->UpdateSubresource(constants.Get(), 0, nullptr, &values, 0, 0);
    ID3D11ShaderResourceView* views[3]{surface.view.Get(), overlay ? blackView.Get() : nullptr, overlay ? whiteView.Get() : nullptr};
    context->PSSetShaderResources(0, 3, views);
    D3D11_VIEWPORT port{float(region.left), float(region.top), float(region.width()), float(region.height()), 0.0f, 1.0f};
    context->OMSetRenderTargets(1, luma.GetAddressOf(), nullptr); context->RSSetViewports(1, &port);
    context->PSSetShader(lumaShader.Get(), nullptr, 0); context->Draw(3, 0);
    port = {float(region.left / 2), float(region.top / 2), float(region.width() / 2), float(region.height() / 2), 0.0f, 1.0f};
    context->OMSetRenderTargets(1, chroma.GetAddressOf(), nullptr); context->RSSetViewports(1, &port);
    context->PSSetShader(chromaShader.Get(), nullptr, 0); context->Draw(3, 0);
  }
  void pipeline() {
    context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0); context->RSSetState(rasterizer.Get());
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff); context->OMSetDepthStencilState(nullptr, 0);
  }
  void unbind() {
    ID3D11ShaderResourceView* none[3]{}; context->PSSetShaderResources(0, 3, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
  }
};

FrameComposer::FrameComposer(ID3D11Device* device, int width, int height, std::vector<capture::PixelRect> surfaces) : impl_(std::make_unique<Impl>()) {
  auto& s = *impl_;
  if (!device || width <= 0 || height <= 0 || (width | height) & 1 || surfaces.empty() || surfaces.size() > 16) throw GpuFailure(GpuStep::target, E_INVALIDARG, "Invalid GPU capture frame");
  for (const auto& rect : surfaces) if (!capture::evenRect(rect) || rect.left < 0 || rect.top < 0 || rect.right > width || rect.bottom > height) throw GpuFailure(GpuStep::target, E_INVALIDARG, "Invalid GPU capture surface");
  s.device = device; device->GetImmediateContext(&s.context); s.width = width; s.height = height;
  UINT support = 0;
  if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D) || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
    throw GpuFailure(GpuStep::formatSupport, E_NOTIMPL, "NV12 render targets unavailable");
  const auto& code = bytecode();
  require(device->CreateVertexShader(code.vertex.data(), code.vertex.size(), nullptr, &s.vertex), GpuStep::shaders, "Capture vertex shader");
  require(device->CreatePixelShader(code.luma.data(), code.luma.size(), nullptr, &s.lumaShader), GpuStep::shaders, "Capture luma shader");
  require(device->CreatePixelShader(code.chroma.data(), code.chroma.size(), nullptr, &s.chromaShader), GpuStep::shaders, "Capture chroma shader");
  s.target = s.texture(width, height, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET, GpuStep::target);
  D3D11_RENDER_TARGET_VIEW_DESC view{}; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  view.Format = DXGI_FORMAT_R8_UNORM; require(device->CreateRenderTargetView(s.target.Get(), &view, &s.luma), GpuStep::views, "NV12 luma view");
  view.Format = DXGI_FORMAT_R8G8_UNORM; require(device->CreateRenderTargetView(s.target.Get(), &view, &s.chroma), GpuStep::views, "NV12 chroma view");
  D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(Constants); buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  require(device->CreateBuffer(&buffer, nullptr, &s.constants), GpuStep::states, "Capture constants");
  D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
  require(device->CreateRasterizerState(&raster, &s.rasterizer), GpuStep::states, "Capture rasterizer");
  // Every texture a tick touches exists before the first frame: surfaceFrame
  // only copies and cannot fail.
  for (const auto& rect : surfaces) {
    Impl::Surface surface; surface.placed = rect;
    surface.latest = s.texture(rect.width(), rect.height(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, GpuStep::surface);
    surface.view = s.view(surface.latest.Get(), GpuStep::surface);
    s.surfaces.push_back(std::move(surface));
  }
  s.canvases(64, 64);
}
FrameComposer::~FrameComposer() = default;
void FrameComposer::surfaceFrame(size_t index, ID3D11Texture2D* frame) {
  auto& s = *impl_;
  if (index >= s.surfaces.size() || !frame) throw std::logic_error("Invalid GPU capture surface frame");
  auto& surface = s.surfaces[index];
  const D3D11_BOX box{0, 0, 0, UINT(surface.placed.width()), UINT(surface.placed.height()), 1};
  s.context->CopySubresourceRegion(surface.latest.Get(), 0, 0, 0, 0, frame, 0, &box);
  surface.valid = surface.dirty = true;
}
bool FrameComposer::hasFrame(size_t index) const { return index < impl_->surfaces.size() && impl_->surfaces[index].valid; }
ID3D11Texture2D* FrameComposer::latest(size_t index) const { return index < impl_->surfaces.size() ? impl_->surfaces[index].latest.Get() : nullptr; }
bool FrameComposer::compose(const CursorOverlay* overlay) {
  auto& s = *impl_;
  for (const auto& surface : s.surfaces) if (!surface.valid) throw GpuFailure(GpuStep::convert, E_UNEXPECTED, "GPU capture surface has no frame");
  if (overlay && (overlay->patch.empty() || !capture::evenRect(overlay->patch) || overlay->patch.right > s.width || overlay->patch.bottom > s.height ||
                  overlay->black.size() != size_t(overlay->patch.width()) * overlay->patch.height() * 4 || overlay->white.size() != overlay->black.size()))
    throw GpuFailure(GpuStep::cursor, E_INVALIDARG, "Invalid GPU cursor overlay");
  bool changed = false, pipelineSet = false;
  const auto ready = [&] { if (!pipelineSet) { s.pipeline(); pipelineSet = true; } };
  std::vector<bool> rewritten(s.surfaces.size(), false);
  for (size_t i = 0; i < s.surfaces.size(); ++i) {
    if (!s.surfaces[i].dirty) continue;
    ready(); s.convert(i, s.surfaces[i].placed, nullptr);
    s.surfaces[i].dirty = false; rewritten[i] = true; changed = true;
  }
  const auto under = [&](const capture::PixelRect& patch, auto&& visit) {
    for (size_t i = 0; i < s.surfaces.size(); ++i) if (const auto cut = capture::intersect(patch, s.surfaces[i].placed)) visit(i, *cut);
  };
  if (s.previous) {
    bool baseRewritten = false;
    under(s.previous->patch, [&](size_t i, const capture::PixelRect&) { baseRewritten = baseRewritten || rewritten[i]; });
    const bool same = overlay && overlay->patch == s.previous->patch && overlay->black == s.previous->black && overlay->white == s.previous->white;
    if (!same || baseRewritten) {
      // The composed cursor of the previous picture goes: its patch gets its
      // base back (a rewritten surface already has it; a redraw is harmless).
      if (restorePreviousCursor) under(s.previous->patch, [&](size_t i, const capture::PixelRect& cut) { ready(); s.convert(i, cut, nullptr); });
      s.previous.reset(); changed = true;
    }
  }
  if (overlay && !s.previous) {
    const auto& patch = overlay->patch;
    s.canvases(patch.width(), patch.height());
    const D3D11_BOX box{0, 0, 0, UINT(patch.width()), UINT(patch.height()), 1};
    s.context->UpdateSubresource(s.black.Get(), 0, &box, overlay->black.data(), UINT(patch.width()) * 4, 0);
    s.context->UpdateSubresource(s.white.Get(), 0, &box, overlay->white.data(), UINT(patch.width()) * 4, 0);
    under(patch, [&](size_t i, const capture::PixelRect& cut) { ready(); s.convert(i, cut, &patch); });
    s.previous = *overlay; changed = true;
  }
  if (pipelineSet) { s.unbind(); s.context->Flush(); }
  return changed;
}
ID3D11Texture2D* FrameComposer::picture() const { return impl_->target.Get(); }
int FrameComposer::width() const { return impl_->width; }
int FrameComposer::height() const { return impl_->height; }
void FrameComposer::read(std::span<uint8_t> nv12) {
  auto& s = *impl_;
  const size_t w = size_t(s.width), h = size_t(s.height);
  if (nv12.size() < w * h * 3 / 2) throw std::logic_error("GPU capture readback buffer too small");
  if (!s.staging) {
    D3D11_TEXTURE2D_DESC desc{}; s.target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    require(s.device->CreateTexture2D(&desc, nullptr, &s.staging), GpuStep::readback, "GPU capture readback texture");
  }
  s.context->CopyResource(s.staging.Get(), s.target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  require(s.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), GpuStep::readback, "GPU capture readback");
  const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
  // NV12 maps as one allocation: the UV plane follows the luma rows at the same pitch.
  for (size_t y = 0; y < h; ++y) std::memcpy(nv12.data() + y * w, bytes + y * mapped.RowPitch, w);
  for (size_t y = 0; y < h / 2; ++y) std::memcpy(nv12.data() + w * h + y * w, bytes + (h + y) * mapped.RowPitch, w);
  s.context->Unmap(s.staging.Get(), 0);
}
}
