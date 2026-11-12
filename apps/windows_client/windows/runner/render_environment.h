#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>

namespace multitap {
// Privacy-safe facts about how a Flutter view renders, for streaming
// diagnostics only: booleans and a refresh rate, never an adapter name or ID.
struct RenderEnvironment {
  bool known = false;     // Flutter's rendering adapter could be inspected
  bool hardware = false;  // a hardware GPU, not WARP / Microsoft Basic Render Driver
};

// WARP and the Basic Render Driver (also behind the Basic Display Adapter and
// indirect virtual displays of a GPU-less VM) rasterize on the CPU. Their
// DXGI adapters do not always carry DXGI_ADAPTER_FLAG_SOFTWARE.
constexpr UINT kMicrosoftVendor = 0x1414, kBasicRenderDevice = 0x008c;
inline bool SoftwareAdapter(UINT vendor, UINT device, UINT flags) {
  return (flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (vendor == kMicrosoftVendor && device == kBasicRenderDevice);
}

inline RenderEnvironment InspectRenderAdapter(IDXGIAdapter* adapter) {
  RenderEnvironment value;
  if (!adapter) return value;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
  DXGI_ADAPTER_DESC1 desc{};
  if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) || FAILED(adapter1->GetDesc1(&desc))) return value;
  value.known = true;
  value.hardware = !SoftwareAdapter(desc.VendorId, desc.DeviceId, desc.Flags);
  return value;
}

// The rate Flutter's Windows embedder paces its frames at: it derives the
// frame interval from DwmGetCompositionTimingInfo, which reports e.g. 32 Hz in
// a Remote Desktop session. 0 when unknown.
inline int64_t CompositionRefreshHz() {
  DWM_TIMING_INFO info{};
  info.cbSize = sizeof(info);
  if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) || !info.rateRefresh.uiNumerator || !info.rateRefresh.uiDenominator) return 0;
  const double hz = double(info.rateRefresh.uiNumerator) / double(info.rateRefresh.uiDenominator);
  return hz >= 1 && hz <= 1000 ? int64_t(std::lround(hz)) : 0;
}

// The client itself runs inside a Remote Desktop session.
inline bool RemoteDesktopSession() { return GetSystemMetrics(SM_REMOTESESSION) != 0; }
}
