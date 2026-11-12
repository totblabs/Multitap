#ifndef MULTITAP_GPU_VIDEO_H_
#define MULTITAP_GPU_VIDEO_H_
// Hardware decode + GPU presentation plumbing for the Windows client (header
// only, so every target that compiles remote_decoder.cpp / remote_engine.cpp
// needs no extra source file).
//
//  * Device     one D3D11 device on one DXGI adapter, the IMFDXGIDeviceManager
//               a DXVA H.264 MFT needs, and an "adapter has an H.264 VLD
//               decoder" probe.
//  * Converter  NV12 (decoder texture or system memory) -> BGRA into a pooled
//               shared texture with a small pixel shader that repeats the CPU
//               converter's integer maths (native/windows_video/color.hpp), so
//               the result is the same pixels, not "close". Frames are handed
//               to Flutter as legacy DXGI shared handles (GpuSurfaceTexture).
//               A shader (not the D3D11 video processor) keeps this working
//               on every feature-level 10+ device including WARP.
//  * Overlay    a BGRA texture holding the lossless tiles; present tiles are
//               copied over the converted frame on the GPU.
//  * Select     adapter priority: Flutter's render adapter (zero copy), then
//               another hardware adapter (discrete before integrated, one NV12
//               readback per frame), then the CPU path.
// Adapter names are never read; only vendor IDs are reported.
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcommon.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "render_environment.h"
#include "video_bounds.h"
#include "video_memory.h"
#include "../../../../native/windows_video/color.hpp"
#include "../../../../native/windows_video/gpu_completion_poll.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")

namespace multitap::gpu {
using Microsoft::WRL::ComPtr;

// D3D11_DECODER_PROFILE_H264_VLD_NOFGT (defined here to avoid linking dxguid).
inline constexpr GUID kH264Vld = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
// Decode tier reported in the stats: 0 = CPU decode, 1 = hardware decode on
// the render adapter, 2 = hardware decode on another adapter.
constexpr int kTierCpu = 0, kTierRender = 1, kTierOther = 2;

struct AdapterInfo {
  LUID luid{};
  UINT vendor = 0, device = 0;
  bool software = true;
  SIZE_T dedicatedVideoMemory = 0;
};
inline bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
inline AdapterInfo Describe(IDXGIAdapter* adapter) {
  AdapterInfo info;
  ComPtr<IDXGIAdapter1> adapter1;
  DXGI_ADAPTER_DESC1 desc{};
  if (!adapter || FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) || FAILED(adapter1->GetDesc1(&desc))) return info;
  info.luid = desc.AdapterLuid; info.vendor = desc.VendorId; info.device = desc.DeviceId;
  info.software = SoftwareAdapter(desc.VendorId, desc.DeviceId, desc.Flags);
  info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
  return info;
}

// A D3D11 device on one adapter. Created once per adapter and shared (weakly
// cached), so a picture-size change that restarts the engine reuses it.
class Device : public std::enable_shared_from_this<Device> {
 public:
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11VideoDevice> video;  // null when the device has no video support
  // The immediate context's own lock (multithread protected below), for a
  // sequence of calls that depends on pipeline state (Converter::Pass).
  ComPtr<ID3D11Multithread> multithread;
  AdapterInfo info;
  uint32_t TextureDimensionLimit()const{
    if(!device)return 0;const auto level=device->GetFeatureLevel();
    // D3D11/10 specification limits of the ACTUAL created device, not a
    // Multitap resolution cap or a decoder-supported rectangle.
    return level>=D3D_FEATURE_LEVEL_11_0?D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION:
      level>=D3D_FEATURE_LEVEL_10_0?8192u:0u;
  }
  uint64_t AvailableTextureMemory()const{
    // WARP/basic-render textures use system memory. This is an explicitly
    // identified software adapter, not a fabricated hardware VRAM budget.
    if(info.software)return AvailableVideoSystemMemory();
    if(!device)return 0;ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter3> budget;
    if(FAILED(device.As(&dxgi))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter.As(&budget)))return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO state{};
    if(FAILED(budget->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&state))||state.CurrentUsage>=state.Budget)return 0;
    return state.Budget-state.CurrentUsage;
  }
  bool TextureFits(uint32_t width,uint32_t height,uint64_t bytes=0)const{
    video::FrameLayout layout{};const auto maximum=TextureDimensionLimit();
    if(!maximum||!video::frameLayout(width,height,layout,false)||width>maximum||height>maximum)return false;
    UINT support=0;if(FAILED(device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM,&support))||
      !(support&D3D11_FORMAT_SUPPORT_TEXTURE2D)||!(support&D3D11_FORMAT_SUPPORT_RENDER_TARGET)||!(support&D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))return false;
    const auto available=AvailableTextureMemory();return available&&(bytes?bytes:uint64_t(layout.rgbaBytes))<=available;
  }
  bool DecodeFits(uint32_t width,uint32_t height,uint64_t bytes=0)const{
    VideoLayout layout{};const auto maximum=TextureDimensionLimit();
    if(!maximum||!CheckedVideoLayout(width,height,layout)||width>maximum||height>maximum||!SupportsH264(width,height))return false;
    const auto available=AvailableTextureMemory();return available&&(bytes?bytes:uint64_t(layout.nv12Bytes))<=available;
  }

  // `adapter` null with warp = the WARP software rasterizer (tests).
  static std::shared_ptr<Device> Create(IDXGIAdapter* adapter, bool warp = false, HRESULT* failure = nullptr,bool cacheDevice=true) {
    static std::mutex mutex;
    static std::map<uint64_t, std::weak_ptr<Device>> cache;
    AdapterInfo info = adapter ? Describe(adapter) : AdapterInfo{};
    const uint64_t key = adapter ? (uint64_t(uint32_t(info.luid.HighPart)) << 32 | info.luid.LowPart) : ~uint64_t(0);
    std::unique_lock<std::mutex> lock(mutex,std::defer_lock);
    // A metadata query owns an isolated temporary device/context. It never
    // holds the real engine's cache mutex through a slow vendor call.
    if(cacheDevice)lock.lock();
    if (cacheDevice&&adapter) if (const auto found = cache.find(key); found != cache.end()) if (auto held = found->second.lock()) return held;
    auto value = std::make_shared<Device>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : (warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
    HRESULT hr = E_FAIL;
    // Video support first (a DXVA decoder device needs it), then without: a
    // device that only converts and shares textures does not.
    for (const UINT flags : {UINT(D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT), UINT(D3D11_CREATE_DEVICE_BGRA_SUPPORT)}) {
      value->device.Reset(); value->context.Reset();
      hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &value->device, nullptr, &value->context);
      if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) { if (failure) *failure = hr; return {}; }
    value->device.As(&value->video);
    if (SUCCEEDED(value->context.As(&value->multithread))) value->multithread->SetMultithreadProtected(TRUE);
    if (!adapter) {
      ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> own;
      if (SUCCEEDED(value->device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&own))) info = Describe(own.Get());
      if (warp) info.software = true;
    }
    value->info = info;
    if (cacheDevice&&adapter) cache[key] = value;
    return value;
  }

  // The manager a DXVA H.264 MFT is given (MFT_MESSAGE_SET_D3D_MANAGER).
  HRESULT Manager(IMFDXGIDeviceManager** out) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    if (!manager_) {
      UINT token = 0;
      HRESULT hr = MFCreateDXGIDeviceManager(&token, &manager_);
      if (SUCCEEDED(hr)) hr = manager_->ResetDevice(device.Get(), token);
      if (FAILED(hr)) { manager_.Reset(); return hr; }
    }
    manager_.CopyTo(out);
    return S_OK;
  }

  // Whether this adapter has a hardware H.264 (VLD, NV12) decoder that takes
  // a width x height stream.
  bool SupportsH264(uint32_t width, uint32_t height) const {
    if (!video) return false;
    const UINT count = video->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < count; ++i) {
      GUID profile{};
      if (FAILED(video->GetVideoDecoderProfile(i, &profile)) || profile != kH264Vld) continue;
      BOOL supported = FALSE;
      if (FAILED(video->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &supported)) || !supported) return false;
      D3D11_VIDEO_DECODER_DESC desc{};
      desc.Guid = profile; desc.SampleWidth = width; desc.SampleHeight = height; desc.OutputFormat = DXGI_FORMAT_NV12;
      UINT configs = 0;
      return SUCCEEDED(video->GetVideoDecoderConfigCount(&desc, &configs)) && configs > 0;
    }
    return false;
  }

 private:
  std::mutex manager_mutex_;
  ComPtr<IMFDXGIDeviceManager> manager_;
};

// One converted picture: a shared BGRA texture of a pool. The last owner
// returns it to the pool.
struct Frame {
  uint32_t width = 0, height = 0;
  ComPtr<ID3D11Texture2D> texture;
  HANDLE shared = nullptr;  // legacy DXGI shared handle (valid while `texture` lives)
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11ShaderResourceView> view;
  std::shared_ptr<Device> owner;
  // CPU copy of the picture as RGBA (the layout of the CPU path): thumbnails,
  // tests. The producer finished its GPU work before publishing.
  bool ReadRgba(std::vector<uint8_t>& rgba) const {
    if (!texture || !owner) return false;
    video::FrameLayout layout{};if(!video::frameLayout(width,height,layout,false)||!VideoAllocationFits(layout.rgbaBytes)||!owner->TextureFits(width,height))return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(owner->device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    owner->context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(owner->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    if(map.RowPitch<layout.stride){owner->context->Unmap(staging.Get(),0);return false;}
    try{rgba.resize(layout.rgbaBytes);}catch(...){owner->context->Unmap(staging.Get(),0);return false;}
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* row = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
      uint8_t* target_row = rgba.data() + size_t(y) * width * 4;
      for (uint32_t x = 0; x < width; ++x) { target_row[x * 4] = row[x * 4 + 2]; target_row[x * 4 + 1] = row[x * 4 + 1]; target_row[x * 4 + 2] = row[x * 4]; target_row[x * 4 + 3] = row[x * 4 + 3]; }
    }
    owner->context->Unmap(staging.Get(), 0);
    return true;
  }
};

// Shared textures of one size. Reuse is first-released-first-reused so a texture
// Flutter has just opened is the last one a later frame overwrites.
struct Pool : std::enable_shared_from_this<Pool> {
  static constexpr size_t kMaximum = 10;
  std::shared_ptr<Device> device;
  uint32_t width = 0, height = 0;
  std::mutex mutex;
  std::deque<std::unique_ptr<Frame>> free;
  size_t total = 0;
  std::shared_ptr<Frame> Acquire() {
    if(!device)return {};
    std::unique_ptr<Frame> frame;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!free.empty()) { frame = std::move(free.front()); free.pop_front(); }
      else if (total < kMaximum) ++total;
      else return {};
    }
    if (!frame) {
      if(!device->TextureFits(width,height)){std::lock_guard<std::mutex> lock(mutex);--total;return {};}
      frame = std::make_unique<Frame>();
      frame->width = width; frame->height = height; frame->owner = device;
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
      ComPtr<IDXGIResource> resource;
      if (FAILED(device->device->CreateTexture2D(&desc, nullptr, &frame->texture)) || FAILED(frame->texture.As(&resource)) ||
          FAILED(resource->GetSharedHandle(&frame->shared)) ||
          FAILED(device->device->CreateRenderTargetView(frame->texture.Get(), nullptr, &frame->target)) ||
          FAILED(device->device->CreateShaderResourceView(frame->texture.Get(), nullptr, &frame->view))) {
        std::lock_guard<std::mutex> lock(mutex); --total; return {};
      }
    }
    return std::shared_ptr<Frame>(frame.release(), [weak = weak_from_this()](Frame* value) noexcept {
      std::unique_ptr<Frame> owned(value);
      try { if (const auto pool = weak.lock()) { std::lock_guard<std::mutex> lock(pool->mutex); pool->free.push_back(std::move(owned)); } } catch (...) {}
    });
  }
};

struct Colour { bool bt709 = false, full_range = false; };

inline const char* ConverterShader() {
  return R"hlsl(
cbuffer Params : register(b0) { int4 a; int4 b; };   // a = (lumaOffset, lumaScale, redV, greenU), b = (greenV, blueU, 0, 0)
Texture2D<float> LumaTex : register(t0);
Texture2D<float2> ChromaTex : register(t1);
Texture2D<float4> Rgb : register(t2);
SamplerState Linear : register(s0);
struct Out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Out VS(uint id : SV_VertexID) {
  Out o; float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o;
}
float4 PSNv12(Out i) : SV_Target {
  int2 xy = int2(i.pos.xy);
  int y = (int)(LumaTex.Load(int3(xy, 0)) * 255.0 + 0.5);
  float2 c = ChromaTex.Load(int3(xy >> 1, 0));
  int u = (int)(c.x * 255.0 + 0.5) - 128, v = (int)(c.y * 255.0 + 0.5) - 128;
  int l = max(0, y - a.x);
  int r = (a.y * l + a.z * v + 128) >> 8;
  int g = (a.y * l - a.w * u - b.x * v + 128) >> 8;
  int bl = (a.y * l + b.y * u + 128) >> 8;
  return float4(clamp(int3(r, g, bl), 0, 255) / 255.0, 1.0);
}
float4 PSScale(Out i) : SV_Target { return float4(Rgb.SampleLevel(Linear, i.uv, 0).rgb, 1.0); }
)hlsl";
}

// Compiles with the system's d3dcompiler (Windows 10+). Null on failure.
inline ComPtr<ID3DBlob> CompileShader(const char* entry, const char* profile) {
  using Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  static const auto compile = [] {
    const HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<Compile>(GetProcAddress(module, "D3DCompile")) : nullptr;
  }();
  // Compiled once per process: the engine restarts on every picture-size change.
  static std::mutex mutex;
  static std::map<std::string, ComPtr<ID3DBlob>> cache;
  std::lock_guard<std::mutex> lock(mutex);
  const std::string key = std::string(entry) + "/" + profile;
  if (const auto found = cache.find(key); found != cache.end()) return found->second;
  ComPtr<ID3DBlob> code, errors;
  if (!compile) return {};
  const char* source = ConverterShader();
  if (FAILED(compile(source, std::strlen(source), "multitap-converter", nullptr, nullptr, entry, profile, 1 << 15 /*optimization level 3*/, 0, &code, &errors))) return {};
  cache[key] = code;
  return code;
}

// NV12 / BGRA -> pooled BGRA shared texture. One instance per picture stream;
// not thread-safe (the decode worker owns it).
class Converter {
 public:
  explicit Converter(std::shared_ptr<Device> device) : device_(std::move(device)) {}
  const std::shared_ptr<Device>& device() const { return device_; }
  double last_ms() const { return last_ms_; }

  // `nv12` is a decoder output (or any NV12 texture) of this converter's
  // device; `slice` its array slice. The visible picture is width x height at
  // the top left; the result is out_width x out_height (bilinear when larger).
  HRESULT FromTexture(ID3D11Texture2D* nv12, UINT slice, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, Colour colour, std::shared_ptr<Frame>& out) {
    const auto began = std::chrono::steady_clock::now();
    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || width > desc.Width || height > desc.Height) return MF_E_INVALIDMEDIATYPE;
    HRESULT hr = EnsureShaders();
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11ShaderResourceView> luma, chroma;
    // Sampling the decoder's own texture (plane views of NV12) when it allows
    // it; otherwise, or when the driver refuses those views, one GPU copy into a
    // texture made for sampling.
    const auto make_views = [&](ID3D11Texture2D* source, UINT view_slice, UINT array_size) -> HRESULT {
      for (const bool plane : {false, true}) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
        if (array_size > 1) {
          view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
          view.Texture2DArray.MipLevels = 1; view.Texture2DArray.FirstArraySlice = view_slice; view.Texture2DArray.ArraySize = 1;
        } else { view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1; }
        const HRESULT made = device_->device->CreateShaderResourceView(source, &view, plane ? &chroma : &luma);
        if (FAILED(made)) return made;
      }
      return S_OK;
    };
    hr = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? make_views(nv12, slice, desc.ArraySize) : E_ACCESSDENIED;
    if (FAILED(hr)) {
      if (!copy_ || copy_width_ != desc.Width || copy_height_ != desc.Height) {
        copy_.Reset();
        D3D11_TEXTURE2D_DESC copy{};
        copy.Width = desc.Width; copy.Height = desc.Height; copy.MipLevels = copy.ArraySize = 1; copy.Format = DXGI_FORMAT_NV12;
        copy.SampleDesc.Count = 1; copy.Usage = D3D11_USAGE_DEFAULT; copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hr = device_->device->CreateTexture2D(&copy, nullptr, &copy_))) return hr;
        copy_width_ = desc.Width; copy_height_ = desc.Height;
      }
      device_->context->CopySubresourceRegion(copy_.Get(), 0, 0, 0, 0, nv12, slice, nullptr);
      if (FAILED(hr = make_views(copy_.Get(), 0, 1))) return hr;
    }    hr = Draw(luma.Get(), chroma.Get(), width, height, out_width, out_height, colour, out);
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    return hr;
  }
  // Planar NV12 in system memory: luma rows then interleaved UV rows, `stride`
  // bytes apart, `plane_height` luma rows (the layout nv12ToRgba takes).
  HRESULT FromSystem(const uint8_t* nv12, uint32_t stride, uint32_t plane_height, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, Colour colour, std::shared_ptr<Frame>& out) {
    const auto began = std::chrono::steady_clock::now();
    if ((stride & 1) || (plane_height & 1) || stride < width || plane_height < height || !width || !height) return E_INVALIDARG;
    HRESULT hr = EnsureShaders();
    if (FAILED(hr)) return hr;
    if (!luma_ || upload_width_ != stride || upload_height_ != plane_height) {
      if(!device_->TextureFits(stride,plane_height))return E_OUTOFMEMORY;
      luma_.Reset(); chroma_.Reset(); luma_view_.Reset(); chroma_view_.Reset();
      D3D11_TEXTURE2D_DESC desc{};
      desc.MipLevels = desc.ArraySize = 1; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.Width = stride; desc.Height = plane_height; desc.Format = DXGI_FORMAT_R8_UNORM;
      if (FAILED(hr = device_->device->CreateTexture2D(&desc, nullptr, &luma_))) return hr;
      desc.Width = stride / 2; desc.Height = plane_height / 2; desc.Format = DXGI_FORMAT_R8G8_UNORM;
      if (FAILED(hr = device_->device->CreateTexture2D(&desc, nullptr, &chroma_))) return hr;
      if (FAILED(hr = device_->device->CreateShaderResourceView(luma_.Get(), nullptr, &luma_view_)) || FAILED(hr = device_->device->CreateShaderResourceView(chroma_.Get(), nullptr, &chroma_view_))) return hr;
      upload_width_ = stride; upload_height_ = plane_height;
    }
    device_->context->UpdateSubresource(luma_.Get(), 0, nullptr, nv12, stride, 0);
    device_->context->UpdateSubresource(chroma_.Get(), 0, nullptr, nv12 + size_t(stride) * plane_height, stride, 0);
    hr = Draw(luma_view_.Get(), chroma_view_.Get(), width, height, out_width, out_height, colour, out);
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    return hr;
  }
  // A BGRA picture (this device's) scaled to out_width x out_height: the
  // republish of a picture that is still the video size under a larger
  // lossless source.
  HRESULT Scale(const Frame& source, uint32_t out_width, uint32_t out_height, std::shared_ptr<Frame>& out) {
    HRESULT hr = EnsureShaders();
    if (FAILED(hr)) return hr;
    if (FAILED(hr = EnsurePool(out_pool_, out_width, out_height))) return hr;
    out = out_pool_->Acquire();
    if (!out) return E_PENDING;
    if (FAILED(hr = Pass(ps_scale_.Get(), nullptr, nullptr, source.view.Get(), *out, Colour{}))) { out.reset(); return hr; }
    if (FAILED(hr = Finish())) { out.reset(); return hr; }
    return S_OK;
  }
  // A new pooled frame with `source`'s pixels (same size).
  HRESULT Clone(const Frame& source, std::shared_ptr<Frame>& out) {
    HRESULT hr = EnsurePool(out_pool_, source.width, source.height);
    if (FAILED(hr)) return hr;
    out = out_pool_->Acquire();
    if (!out) return E_PENDING;
    device_->context->CopyResource(out->texture.Get(), source.texture.Get());
    if (FAILED(hr = Finish())) { out.reset(); return hr; }
    return S_OK;
  }
  // Waits until the GPU finished everything submitted so far (a shared texture
  // is read by Flutter's own device, without a keyed mutex).
  HRESULT Finish() {
    if (!query_) {
      D3D11_QUERY_DESC desc{}; desc.Query = D3D11_QUERY_EVENT;
      HRESULT hr = device_->device->CreateQuery(&desc, &query_);
      if (FAILED(hr)) return hr;
    }
    HRESULT status = S_OK;
    const auto result = video::waitForGpuCompletion(std::chrono::seconds(2),
      [] { return std::chrono::steady_clock::now(); },
      [&] { device_->context->End(query_.Get()); device_->context->Flush(); },
      [&](video::GpuCompletionPollMode) {
        BOOL done = FALSE;
        // Flush above already submits the marker. flags0 can cause another
        // driver flush on each pending observation, serializing both tracks'
        // shared worker further. Never publish before actual query completion.
        status = device_->context->GetData(query_.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        return FAILED(status) ? video::GpuCompletionState::failed :
          status == S_OK && done ? video::GpuCompletionState::ready : video::GpuCompletionState::pending;
      },
      [] { SwitchToThread(); });
    if (result == video::GpuCompletionResult::ready) return S_OK;
    if (result == video::GpuCompletionResult::failed) return status;
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }

 private:
  HRESULT EnsurePool(std::shared_ptr<Pool>& slot, uint32_t width, uint32_t height) {
    video::FrameLayout layout{};if (!video::frameLayout(width,height,layout,false)) return E_INVALIDARG;
    if (slot && slot->width == width && slot->height == height) return S_OK;
    if(!device_->TextureFits(width,height))return E_OUTOFMEMORY;
    slot = std::make_shared<Pool>();
    slot->device = device_; slot->width = width; slot->height = height;
    return S_OK;
  }
  HRESULT EnsureShaders() {
    if (ps_nv12_) return S_OK;
    const auto vs = CompileShader("VS", "vs_4_0"), nv12 = CompileShader("PSNv12", "ps_4_0"), scale = CompileShader("PSScale", "ps_4_0");
    if (!vs || !nv12 || !scale) return D3D11_ERROR_FILE_NOT_FOUND;
    HRESULT hr = device_->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = device_->device->CreatePixelShader(scale->GetBufferPointer(), scale->GetBufferSize(), nullptr, &ps_scale_);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 32; buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = device_->device->CreateBuffer(&buffer, nullptr, &constants_);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device_->device->CreateSamplerState(&sampler, &sampler_);
    if (SUCCEEDED(hr)) hr = device_->device->CreatePixelShader(nv12->GetBufferPointer(), nv12->GetBufferSize(), nullptr, &ps_nv12_);
    return hr;
  }
  // NV12 views -> out; two passes (convert at the picture size, then scale) when
  // the output is another size.
  HRESULT Draw(ID3D11ShaderResourceView* luma, ID3D11ShaderResourceView* chroma, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, Colour colour, std::shared_ptr<Frame>& out) {
    HRESULT hr;
    const bool scaled = out_width != width || out_height != height;
    if (FAILED(hr = EnsurePool(scaled ? video_pool_ : out_pool_, width, height))) return hr;
    if (scaled && FAILED(hr = EnsurePool(out_pool_, out_width, out_height))) return hr;
    auto converted = (scaled ? video_pool_ : out_pool_)->Acquire();
    if (!converted) return E_PENDING;  // every pooled texture is still referenced
    if (FAILED(hr = Pass(ps_nv12_.Get(), luma, chroma, nullptr, *converted, colour))) return hr;
    if (scaled) {
      out = out_pool_->Acquire();
      if (!out) return E_PENDING;
      if (FAILED(hr = Pass(ps_scale_.Get(), nullptr, nullptr, converted->view.Get(), *out, colour))) { out.reset(); return hr; }
    } else out = std::move(converted);
    if (FAILED(hr = Finish())) { out.reset(); return hr; }
    return S_OK;
  }
  HRESULT Pass(ID3D11PixelShader* shader, ID3D11ShaderResourceView* luma, ID3D11ShaderResourceView* chroma, ID3D11ShaderResourceView* rgb, const Frame& target, Colour colour) {
    auto* context = device_->context.Get();
    // Both tracks-v1 outputs may convert on this device at the same time, each on
    // its own decode lane: multithread protection makes each call atomic, not
    // this sequence of state + draw, so another lane's Pass must not interleave.
    // Only the recording is held; the completion wait (Finish) runs unlocked.
    struct Held { ID3D11Multithread* lock; explicit Held(ID3D11Multithread* value) : lock(value) { if (lock) lock->Enter(); } ~Held() { if (lock) lock->Leave(); } } held(device_->multithread.Get());
    // The CPU converter's integer coefficients (native/windows_video/color.hpp).
    const int lumaScale = colour.full_range ? 256 : 298;
    const int redV = colour.full_range ? (colour.bt709 ? 403 : 359) : (colour.bt709 ? 459 : 409);
    const int greenU = colour.full_range ? (colour.bt709 ? 48 : 88) : (colour.bt709 ? 55 : 100);
    const int greenV = colour.full_range ? (colour.bt709 ? 120 : 183) : (colour.bt709 ? 136 : 208);
    const int blueU = colour.full_range ? (colour.bt709 ? 475 : 454) : (colour.bt709 ? 541 : 516);
    const int params[8] = {colour.full_range ? 0 : 16, lumaScale, redV, greenU, greenV, blueU, 0, 0};
    context->UpdateSubresource(constants_.Get(), 0, nullptr, params, 0, 0);
    ID3D11ShaderResourceView* views[3] = {luma, chroma, rgb};
    ID3D11RenderTargetView* targets[1] = {target.target.Get()};
    const D3D11_VIEWPORT viewport{0, 0, float(target.width), float(target.height), 0, 1};
    context->OMSetRenderTargets(1, targets, nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->PSSetShader(shader, nullptr, 0);
    ID3D11Buffer* buffers[1] = {constants_.Get()};
    context->PSSetConstantBuffers(0, 1, buffers);
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    context->PSSetSamplers(0, 1, samplers);
    context->PSSetShaderResources(0, 3, views);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
    context->PSSetShaderResources(0, 3, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return S_OK;
  }
  std::shared_ptr<Device> device_;
  std::shared_ptr<Pool> out_pool_, video_pool_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_nv12_, ps_scale_;
  ComPtr<ID3D11Buffer> constants_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11Texture2D> luma_, chroma_, copy_;
  ComPtr<ID3D11ShaderResourceView> luma_view_, chroma_view_;
  uint32_t upload_width_ = 0, upload_height_ = 0, copy_width_ = 0, copy_height_ = 0;
  ComPtr<ID3D11Query> query_;
  double last_ms_ = 0;
};

// Lossless tiles on the GPU. Tile pixels are kept as BGRA in a texture of the
// lossless source size; present tiles are copied over a converted frame.
class Overlay {
 public:
  explicit Overlay(std::shared_ptr<Device> device) : device_(std::move(device)) {}
  HRESULT Configure(uint32_t width, uint32_t height) {
    video::FrameLayout layout{};if(!video::frameLayout(width,height,layout,false))return E_INVALIDARG;
    if (texture_ && width_ == width && height_ == height) return S_OK;
    if(!device_->TextureFits(width,height))return E_OUTOFMEMORY;
    texture_.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->device->CreateTexture2D(&desc, nullptr, &texture_);
    if (SUCCEEDED(hr)) { width_ = width; height_ = height; }
    return hr;
  }
  // `rgba` points at the tile's first pixel inside the compositor's RGBA layer.
  void UploadTile(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgba, size_t pitch) {
    scratch_.resize(size_t(width) * height * 4);
    for (uint32_t row = 0; row < height; ++row) {
      const uint8_t* s = rgba + size_t(row) * pitch; uint8_t* t = scratch_.data() + size_t(row) * width * 4;
      for (uint32_t col = 0; col < width; ++col) { t[col * 4] = s[col * 4 + 2]; t[col * 4 + 1] = s[col * 4 + 1]; t[col * 4 + 2] = s[col * 4]; t[col * 4 + 3] = s[col * 4 + 3]; }
    }
    const D3D11_BOX box{x, y, 0, x + width, y + height, 1};
    device_->context->UpdateSubresource(texture_.Get(), 0, &box, scratch_.data(), width * 4, 0);
  }
  // Copies a run of the overlay over `frame` (same coordinates).
  void CopyRun(const Frame& frame, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const D3D11_BOX box{x, y, 0, x + width, y + height, 1};
    device_->context->CopySubresourceRegion(frame.texture.Get(), 0, x, y, 0, texture_.Get(), 0, &box);
  }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  std::shared_ptr<Device> device_;
  ComPtr<ID3D11Texture2D> texture_;
  uint32_t width_ = 0, height_ = 0;
  std::vector<uint8_t> scratch_;
};

// What a stream uses.
struct Selection {
  std::shared_ptr<Device> render;  // converts + owns the shared textures; null: CPU presentation
  std::shared_ptr<Device> decode;  // hardware decoder device; null: CPU decode
  int tier = kTierCpu;
  UINT render_vendor = 0, decode_vendor = 0;
};

struct SelectionOptions {
  bool hardware_decode = true;         // the user's setting
  bool gpu_present = true;             // false: CPU pixel buffer (keeps the CPU path)
  bool allow_software_render = false;  // tests: present on WARP / basic render adapters
  bool warp_render = false;            // tests: no Flutter adapter, create WARP
  bool isolated_query = false;         // uncached device/context for bounded metadata queries
};

// `flutter_adapter` is the adapter Flutter renders this window on (null when
// unknown). Priority: (1) that adapter, if it decodes H.264 in hardware;
// (2) another non-software adapter that does (more dedicated video memory
// first); (3) CPU decode. Presentation on the GPU needs a hardware render
// adapter (or the test override); otherwise frames are converted on the CPU.
inline Selection Select(IDXGIAdapter* flutter_adapter, uint32_t width, uint32_t height, const SelectionOptions& options) {
  Selection selection;
  std::shared_ptr<Device> render;
  if (flutter_adapter) render = Device::Create(flutter_adapter,false,nullptr,!options.isolated_query);
  else if (options.warp_render) render = Device::Create(nullptr, true,nullptr,!options.isolated_query);
  if (render) selection.render_vendor = render->info.vendor;
  const bool render_hardware = render && !render->info.software;
  if (options.gpu_present && render && (render_hardware || options.allow_software_render)) selection.render = render;
  if (!options.hardware_decode) return selection;
  if (render_hardware && render->SupportsH264(width, height)) {
    selection.decode = render; selection.tier = kTierRender; selection.decode_vendor = render->info.vendor;
    return selection;
  }
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return selection;
  std::vector<std::shared_ptr<Device>> candidates;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    const AdapterInfo info = Describe(adapter.Get());
    if (info.software || (render && SameLuid(info.luid, render->info.luid))) continue;
    if (auto device = Device::Create(adapter.Get(),false,nullptr,!options.isolated_query); device && device->SupportsH264(width, height)) candidates.push_back(std::move(device));
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a->info.dedicatedVideoMemory > b->info.dedicatedVideoMemory; });
  if (!candidates.empty()) {
    selection.decode = candidates.front(); selection.tier = kTierOther; selection.decode_vendor = selection.decode->info.vendor;
  }
  return selection;
}

// A conversion of a known picture must come back exactly as the CPU converter
// produces it before a device presents anything; otherwise the CPU path is used.
inline bool ConversionSelfTest(Converter& converter) {
  constexpr uint32_t width = 32, height = 16;
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2), reference(size_t(width) * height * 4);
  for (uint32_t i = 0; i < width * height; ++i) nv12[i] = uint8_t(16 + (i * 7) % 220);
  for (size_t i = size_t(width) * height; i < nv12.size(); ++i) nv12[i] = uint8_t(96 + (i * 5) % 64);
  video::nv12ToRgba(nv12.data(), reference.data(), width, height, width, height, false, false);
  std::shared_ptr<Frame> frame;
  std::vector<uint8_t> rgba;
  return SUCCEEDED(converter.FromSystem(nv12.data(), width, height, width, height, width, height, Colour{}, frame)) && frame &&
         frame->ReadRgba(rgba) && rgba == reference;
}

// The DXGI adapter that drives the monitor under `bounds` (virtual-desktop
// pixels; the monitor holding most of the rectangle). Null when no adapter
// reports that monitor, e.g. a remote session or a basic display path.
inline ComPtr<IDXGIAdapter> AdapterForRect(const RECT& bounds) {
  const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL);
  if (!monitor) return {};
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT output_index = 0;; ++output_index) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC desc{};
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        ComPtr<IDXGIAdapter> result;
        adapter.As(&result);
        return result;
      }
    }
  }
  return {};
}

// One tracks-v1 output (its own decoder and, on the GPU path, its own
// presentation device): (1) the adapter that drives the output's window, when it
// is a hardware adapter with an H.264 decoder for this size - the frames then
// never leave the adapter the window is shown on; otherwise (2)..(3) the usual
// Select order from Flutter's render adapter (render adapter, another hardware
// adapter, CPU). `monitor_adapter` null: Select. The user's hardware-decoding
// setting off keeps every output on the CPU (options.hardware_decode false).
inline Selection SelectForTrack(IDXGIAdapter* monitor_adapter, IDXGIAdapter* render_adapter, uint32_t width, uint32_t height, const SelectionOptions& options) {
  if (monitor_adapter && options.hardware_decode) {
    const AdapterInfo info = Describe(monitor_adapter);
    const bool same = render_adapter && SameLuid(info.luid, Describe(render_adapter).luid);
    if (!info.software && !same) {
      if (auto device = Device::Create(monitor_adapter,false,nullptr,!options.isolated_query); device && device->SupportsH264(width, height)) {
        Selection selection;
        selection.decode = device; selection.tier = kTierRender; selection.decode_vendor = info.vendor;
        selection.render_vendor = info.vendor;
        if (options.gpu_present) selection.render = device;
        return selection;
      }
    }
  }
  return Select(render_adapter, width, height, options);
}
}  // namespace multitap::gpu
#endif
