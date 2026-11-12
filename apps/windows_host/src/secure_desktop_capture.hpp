#pragma once
// DXGI Desktop Duplication of the stream's one owned output for the Role::secure
// helper, using Sunshine's sequence (the thread attaches to the current input
// desktop with OpenInputDesktop + SetThreadDesktop, then DuplicateOutput; a lost
// duplication is opened again). The mouse pointer is composed into the frame,
// because the secure desktop's pointer is not part of the duplicated image.
// Proven live by secure_desktop_probe.cpp.
//
// The capture binds to exactly the output whose adapter LUID and desktop
// rectangle the service passed; any other output is a mismatch, never a
// substitute.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "secure_desktop_helper.hpp"

namespace host::secure {

// The calling thread joins the current input desktop (it must own no window and
// no hook, which holds for the helper's threads). only: join it only when it has
// that name ("Winlogon" for the secure-desktop helper, "Default" for the system
// injector); `other` then says it was another one.
inline bool attachInputDesktop(const wchar_t* only = nullptr, bool* other = nullptr) {
  if (other) *other = false;
  HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
  if (!input) return false;
  bool ok = true;
  if (only) {
    wchar_t name[64] = {}; DWORD needed = 0;
    ok = GetUserObjectInformationW(input, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed) && _wcsicmp(name, only) == 0;
    if (!ok && other) *other = true;
  }
  ok = ok && SetThreadDesktop(input) != 0;
  CloseDesktop(input);
  return ok;
}

// The union of rectangles (empty input: an empty rectangle).
inline RECT unionOf(const std::vector<RECT>& rects) {
  RECT all{};
  bool first = true;
  for (const RECT& r : rects) {
    if (r.right <= r.left || r.bottom <= r.top) continue;
    if (first) { all = r; first = false; continue; }
    all.left = (std::min)(all.left, r.left); all.top = (std::min)(all.top, r.top);
    all.right = (std::max)(all.right, r.right); all.bottom = (std::max)(all.bottom, r.bottom);
  }
  return all;
}
// The Windows virtual desktop in PHYSICAL pixels: the union of every output
// attached to the desktop. SendInput's absolute positions are fractions of this
// rectangle. DXGI's coordinates are never DPI-virtualized; GetSystemMetrics in a
// process Windows treats as DPI-unaware is (a 1920x1080 output at 125 % reads as
// 1536x864, and every position of the real output would then be refused as
// outside the desktop). Empty when DXGI cannot be asked.
inline RECT physicalDesktopBounds() {
  using Microsoft::WRL::ComPtr;
  std::vector<RECT> rects;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC od{};
      if (SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop) rects.push_back(od.DesktopCoordinates);
    }
  }
  return unionOf(rects);
}
// Compose a DXGI pointer shape onto a BGRA picture at (px,py). Colour, masked
// colour and monochrome shapes; clipped to the picture; a shape buffer shorter
// than its own description is ignored.
inline void composePointer(std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, int px, int py,
                           const std::vector<uint8_t>& shape, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info) {
  if (shape.empty() || !info.Width || !info.Height || info.Width > 1024 || info.Height > 2048) return;
  if (bgra.size() < size_t(w) * h * 4) return;
  const size_t pitch = info.Pitch;
  if (!pitch || shape.size() < pitch * info.Height) return;
  if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
      info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
    if (pitch < size_t(info.Width) * 4) return;
    for (uint32_t y = 0; y < info.Height; ++y) {
      const int dy = py + int(y);
      if (dy < 0 || dy >= int(h)) continue;
      const uint8_t* src = shape.data() + size_t(y) * pitch;
      uint8_t* dst = bgra.data() + (size_t(dy) * w) * 4;
      for (uint32_t x = 0; x < info.Width; ++x) {
        const int dx = px + int(x);
        if (dx < 0 || dx >= int(w)) continue;
        const uint8_t* s = src + size_t(x) * 4;
        uint8_t* d = dst + size_t(dx) * 4;
        if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
          const uint8_t a = s[3];
          if (!a) continue;
          d[0] = uint8_t((s[0] * a + d[0] * (255 - a)) / 255);
          d[1] = uint8_t((s[1] * a + d[1] * (255 - a)) / 255);
          d[2] = uint8_t((s[2] * a + d[2] * (255 - a)) / 255);
        } else {  // masked colour: the alpha byte is a mask flag (0 copy, 0xff XOR)
          if (s[3] == 0) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
          else { d[0] ^= s[0]; d[1] ^= s[1]; d[2] ^= s[2]; }
        }
      }
    }
    return;
  }
  if (info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) return;
  // Monochrome: top half AND mask, bottom half XOR mask, 1 bit per pixel.
  if (pitch < (size_t(info.Width) + 7) / 8) return;
  const uint32_t half = info.Height / 2;
  for (uint32_t y = 0; y < half; ++y) {
    const int dy = py + int(y);
    if (dy < 0 || dy >= int(h)) continue;
    uint8_t* dst = bgra.data() + (size_t(dy) * w) * 4;
    for (uint32_t x = 0; x < info.Width; ++x) {
      const int dx = px + int(x);
      if (dx < 0 || dx >= int(w)) continue;
      const uint8_t andMask = (shape[size_t(y) * pitch + (x / 8)] >> (7 - (x % 8))) & 1;
      const uint8_t xorMask = (shape[size_t(y + half) * pitch + (x / 8)] >> (7 - (x % 8))) & 1;
      uint8_t* d = dst + size_t(dx) * 4;
      if (andMask == 0 && xorMask == 0) { d[0] = d[1] = d[2] = 0; }
      else if (andMask == 0 && xorMask == 1) { d[0] = d[1] = d[2] = 255; }
      else if (andMask == 1 && xorMask == 1) { d[0] = uint8_t(~d[0]); d[1] = uint8_t(~d[1]); d[2] = uint8_t(~d[2]); }
      // andMask 1, xorMask 0: transparent.
    }
  }
}

class OutputCapture {
 public:
  explicit OutputCapture(const OwnedOutput& output) : output_(output) {}

  // Open (or open again) the duplication of exactly the owned output. Windows
  // may refuse a duplication for a moment while the desktop switches, so a
  // refusal is tried again a few times. lost: the input desktop is not Winlogon
  // (the caller's scope check decides what that means); nothing was opened.
  CaptureStep open() {
    for (int attempt = 0;; ++attempt) {
      const auto step = openOnce();
      if ((step != CaptureStep::failed && step != CaptureStep::mismatch) || attempt >= 9) return step;
      Sleep(50);
    }
  }

  // Wait up to waitMs for a change (picture or pointer). frame: `bgra` holds the
  // owned output with the pointer composed. lost: the duplication is gone (the
  // desktop switched); open() again after the scope was checked.
  CaptureStep acquire(std::vector<uint8_t>& bgra, UINT waitMs = 16) {
    using Microsoft::WRL::ComPtr;
    if (!dup_) return CaptureStep::lost;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = dup_->AcquireNextFrame(waitMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CaptureStep::none;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) { close(); return CaptureStep::lost; }
    if (FAILED(hr)) return CaptureStep::failed;
    struct Release { IDXGIOutputDuplication* dup; ~Release() { dup->ReleaseFrame(); } } release{dup_.Get()};

    bool changed = false;
    if (info.LastMouseUpdateTime.QuadPart) {
      const bool visible = info.PointerPosition.Visible != 0;
      if (visible != pointerVisible_ || (visible && (info.PointerPosition.Position.x != pointerX_ || info.PointerPosition.Position.y != pointerY_))) changed = true;
      pointerVisible_ = visible;
      if (visible) { pointerX_ = info.PointerPosition.Position.x; pointerY_ = info.PointerPosition.Position.y; }
    }
    if (info.PointerShapeBufferSize && info.PointerShapeBufferSize <= 16u * 1024 * 1024) {
      std::vector<uint8_t> shape(info.PointerShapeBufferSize);
      UINT required = 0;
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
      if (SUCCEEDED(dup_->GetFramePointerShape(UINT(shape.size()), shape.data(), &required, &shapeInfo))) {
        pointer_ = std::move(shape); pointerInfo_ = shapeInfo; changed = true;
      }
    }
    // The picture itself: only when Windows presented something new, or none is held yet.
    if (info.LastPresentTime.QuadPart || !haveBase_) {
      ComPtr<ID3D11Texture2D> texture;
      if (!resource || FAILED(resource.As(&texture)) || !texture) return haveBase_ ? CaptureStep::none : CaptureStep::failed;
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      if (desc.Width != output_.width || desc.Height != output_.height) return CaptureStep::mismatch;
      if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return CaptureStep::failed;
      if (!staging_) {
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging_)) || !staging_) return CaptureStep::failed;
      }
      context_->CopyResource(staging_.Get(), texture.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return CaptureStep::failed;
      if (mapped.RowPitch < desc.Width * 4) { context_->Unmap(staging_.Get(), 0); return CaptureStep::failed; }
      base_.resize(size_t(desc.Width) * desc.Height * 4);
      const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
      for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(base_.data() + size_t(y) * desc.Width * 4, pixels + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 4);
      context_->Unmap(staging_.Get(), 0);
      haveBase_ = true; changed = true;
    }
    if (!haveBase_ || !changed) return CaptureStep::none;
    bgra = base_;
    if (pointerVisible_ && !pointer_.empty()) composePointer(bgra, output_.width, output_.height, pointerX_, pointerY_, pointer_, pointerInfo_);
    return CaptureStep::frame;
  }

  void close() { dup_.Reset(); staging_.Reset(); context_.Reset(); device_.Reset(); haveBase_ = false; }

 private:
  CaptureStep openOnce() {
    using Microsoft::WRL::ComPtr;
    close();
    bool other = false;
    if (!attachInputDesktop(L"Winlogon", &other)) return other ? CaptureStep::lost : CaptureStep::failed;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return CaptureStep::failed;
    ComPtr<IDXGIAdapter1> adapter, found;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
      DXGI_ADAPTER_DESC1 ad{};
      if (FAILED(adapter->GetDesc1(&ad))) continue;
      if (ad.AdapterLuid.LowPart == output_.adapterLow && ad.AdapterLuid.HighPart == output_.adapterHigh) { found = adapter; break; }
    }
    if (!found) return CaptureStep::mismatch;
    ComPtr<IDXGIOutput> output, owned;
    for (UINT o = 0; found->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC od{};
      if (FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop) continue;
      const RECT& r = od.DesktopCoordinates;
      if (r.left == output_.left && r.top == output_.top &&
          int64_t(r.right) - r.left == int64_t(output_.width) && int64_t(r.bottom) - r.top == int64_t(output_.height)) { owned = output; break; }
    }
    if (!owned) return CaptureStep::mismatch;
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(owned.As(&output1))) return CaptureStep::failed;
    if (FAILED(D3D11CreateDevice(found.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_)))
      return CaptureStep::failed;
    if (FAILED(output1->DuplicateOutput(device_.Get(), &dup_)) || !dup_) return CaptureStep::failed;
    return CaptureStep::frame;
  }
  OwnedOutput output_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  std::vector<uint8_t> base_, pointer_;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerInfo_{};
  int pointerX_ = 0, pointerY_ = 0;
  bool pointerVisible_ = false, haveBase_ = false;
};

}  // namespace host::secure
