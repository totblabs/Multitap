#pragma once
#include "support.hpp"
#include "gpu_h264.hpp"
#include <virtual_display/driver/control_client.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <functional>
#include <set>

namespace host {
namespace render_api=virtual_display::driver;
struct NativeRenderPreference {
  const char* status="skipped";
  const char* reason="not_attempted";
  std::optional<uint64_t> requested;
  uint32_t nativeError=0;
};
inline bool nativeRenderOutputsAbsent(const render_api::QueryDisplayStateResult& state){
  return render_api::is_valid_api_namespace(state.api_namespace)&&!state.reserved&&!state.permanent_display_count&&!state.temporary_display_count&&!state.entry_count;
}
// Inventory is not an open-device proof. The probe must return the SAME LUID.
inline std::optional<uint64_t> chooseNativeRenderAdapter(std::span<const gpu::Adapter> adapters,
    const std::function<std::optional<uint64_t>(const gpu::Adapter&)>& probe){
  if(adapters.size()>256||!probe)return {};
  std::set<uint64_t> seen;for(const auto& adapter:adapters)if(gpu::vendorFromId(adapter.vendorId)!=gpu::Vendor::unknown&&(!adapter.luid||!seen.insert(adapter.luid).second))return {};
  size_t tried=0;for(const auto& adapter:adapters){
    // Basic Render Driver can lack DXGI's SOFTWARE flag on a virtual machine.
    if(gpu::vendorFromId(adapter.vendorId)==gpu::Vendor::unknown)continue;
    if(tried++==3)break;
    try{const auto opened=probe(adapter);if(opened&&*opened==adapter.luid)return opened;}catch(...){ }
  }
  return {};
}
inline std::optional<uint64_t> openNativeRenderAdapter(const gpu::Adapter& requested){
  if(gpu::vendorFromId(requested.vendorId)==gpu::Vendor::unknown)return {};
  Com<IDXGIFactory4> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return {};
  const LUID luid{DWORD(requested.luid),LONG(uint32_t(requested.luid>>32))};
  Com<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter))))return {};
  const auto same=[&](const DXGI_ADAPTER_DESC1& d){return !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&d.AdapterLuid.LowPart==luid.LowPart&&d.AdapterLuid.HighPart==luid.HighPart&&d.VendorId==requested.vendorId&&d.DeviceId==requested.deviceId;};
  DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc))||!same(desc))return {};
  Com<ID3D11Device> device;if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)))return {};
  Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> actual;Com<IDXGIAdapter1> checked;
  if(FAILED(device.As(&dxgi))||FAILED(dxgi->GetAdapter(&actual))||FAILED(actual.As(&checked))||FAILED(checked->GetDesc1(&desc))||!same(desc))return {};
  return requested.luid;
}
inline std::optional<uint64_t> chooseNativeRenderHardware(){
  const auto adapters=gpu::enumerateAdapters();return chooseNativeRenderAdapter(adapters,openNativeRenderAdapter);
}
// Cold initialization only, while the caller owns/serializes its controller.
// The SDK has no atomic query-and-set or actual render-device readback.
inline NativeRenderPreference preferNativeRenderAdapter(render_api::ControlClient& client,
    const std::function<std::optional<uint64_t>()>& choose){
  NativeRenderPreference result;
  try{
    const auto protocol=client.check_protocol_compatible();
    if(!protocol.ok())return {"failed","protocol_unavailable",{},protocol.native_error};
    const auto before=client.query_display_state();
    if(!before.ok())return {"failed","state_unavailable",{},before.native_error};
    if(before.value.reserved)return {"failed","state_invalid"};
    if(!nativeRenderOutputsAbsent(before.value))return {"skipped","outputs_present"};
    const auto candidate=choose?choose():std::nullopt;
    if(!candidate||!*candidate)return {"skipped","no_usable_hardware_adapter"};
    result.requested=candidate;
    const auto current=client.query_display_state();
    if(!current.ok())return {"failed","state_unavailable",candidate,current.native_error};
    if(current.value.reserved)return {"failed","state_invalid",candidate};
    if(!nativeRenderOutputsAbsent(current.value))return {"skipped","outputs_present",candidate};
    render_api::SetRenderAdapterRequest request{};
    request.adapter_luid={uint32_t(*candidate),int32_t(uint32_t(*candidate>>32))};
    const auto accepted=client.set_render_adapter(request);
    return {accepted.ok()?"accepted":"failed",accepted.ok()?"request_acknowledged":"request_failed",candidate,accepted.native_error};
  }catch(...){result.status="failed";result.reason="selection_or_transport_failed";return result;}
}
// hostAction 282: render adapter re-arm (NativeDisplayService::requestRenderRearm).
// A worker reported its capture on WARP (hostAction 200 stage 243 reason 2,
// gpu_capture.hpp) although this preference had been accepted; measured
// after a Host update, where a plain service restart put
// the stream back on NVIDIA. stage 1 requested (latched: one per service
// lifetime), 2 performed (ok: the SDK stop was orderly), 3 skipped (error:
// the reason below; a waiting reason is recorded once per change).
inline constexpr uint32_t nativeRenderRearmAction=282,nativeRenderRearmRequested=1,nativeRenderRearmPerformed=2,nativeRenderRearmSkipped=3;
enum NativeRenderRearmSkip:uint32_t {
  nativeRenderRearmAlreadyUsed=1,            // the one re-arm of this service lifetime was already requested
  nativeRenderRearmPreferenceNotAccepted=2,  // the SDK never accepted a hardware adapter: nothing to re-apply
  nativeRenderRearmNoIdleProbe=3,            // no bridge idle probe was installed (fail closed)
  nativeRenderRearmStreamActive=4,           // waiting: a stream, console, recovery or worker holds the display
  nativeRenderRearmDisplayHeld=5,            // waiting: a reservation/profile hold (the tick's hold path)
  nativeRenderRearmOperationPending=6,       // waiting: no live stable controller, or a change/recovery is pending
  nativeRenderRearmNoHardwareAdapter=7,      // chooseNativeRenderHardware found nothing to re-apply
  nativeRenderRearmLeaseNotCurrent=8,        // waiting: the held lease is not current right now
};
inline std::string nativeRenderPreferenceJson(const NativeRenderPreference& value){
  return "{\"status\":"+jsonQuote(value.status)+",\"reason\":"+jsonQuote(value.reason)+",\"requestedAdapterLuid\":"+(value.requested?jsonQuote(gpu::adapterLuidText(*value.requested)):"null")+",\"nativeError\":"+std::to_string(value.nativeError)+",\"actualRenderAdapterLuid\":null,\"warp\":null,\"actualVerified\":null}";
}
}
