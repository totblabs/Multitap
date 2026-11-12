#include "signin_capture_dxgi.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

namespace host::signin::native {
namespace {
using Microsoft::WRL::ComPtr;
Status ready(){return {Outcome::ready,Step::none,0};}
Status hresult(Step step,HRESULT value){
  if(value==DXGI_ERROR_ACCESS_LOST||value==DXGI_ERROR_SESSION_DISCONNECTED)return {Outcome::handoffRequired,step,uint32_t(value)};
  if(value==DXGI_ERROR_UNSUPPORTED||value==E_NOINTERFACE)return {Outcome::unsupported,step,uint32_t(value)};
  return {value==E_ACCESSDENIED?Outcome::refused:Outcome::failed,step,uint32_t(value)};
}
void require(HRESULT value,Step step){if(FAILED(value))throw NativeFailure{hresult(step,value)};}
bool same(LUID a,const OwnedTarget& target){return a.LowPart==target.adapterLow&&a.HighPart==target.adapterHigh;}
bool equalRect(const RECT& value,const OwnedTarget& target){
  return int64_t(value.right)-value.left==target.width&&int64_t(value.bottom)-value.top==target.height&&value.left==target.left&&value.top==target.top;
}
std::wstring ownedSourceName(const OwnedTarget& target){
  for(unsigned attempt=0;attempt<3;++attempt){
    UINT32 pathCount=0,modeCount=0;const auto flags=QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE;
    LONG error=GetDisplayConfigBufferSizes(flags,&pathCount,&modeCount);
    if(error!=ERROR_SUCCESS)throw NativeFailure{{Outcome::refused,Step::output,uint32_t(error)}};
    if(pathCount!=1||modeCount>256)throw NativeFailure{{Outcome::refused,Step::output,0}};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    error=QueryDisplayConfig(flags,&pathCount,paths.data(),&modeCount,modes.data(),nullptr);
    if(error==ERROR_INSUFFICIENT_BUFFER)continue;
    if(error!=ERROR_SUCCESS)throw NativeFailure{{Outcome::refused,Step::output,uint32_t(error)}};
    if(pathCount!=1||!(paths[0].flags&DISPLAYCONFIG_PATH_ACTIVE))throw NativeFailure{{Outcome::refused,Step::output,0}};
    const auto& path=paths[0];
    if(!path.targetInfo.targetAvailable||!same(path.targetInfo.adapterId,target)||!same(path.sourceInfo.adapterId,target)||path.targetInfo.id!=target.targetId||path.sourceInfo.id!=target.sourceId)
      throw NativeFailure{{Outcome::refused,Step::output,0}};
    if(path.targetInfo.rotation!=DISPLAYCONFIG_ROTATION_IDENTITY)throw NativeFailure{{Outcome::unsupported,Step::output,0}};
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),path.sourceInfo.adapterId,path.sourceInfo.id};
    error=DisplayConfigGetDeviceInfo(&source.header);
    if(error!=ERROR_SUCCESS||!source.viewGdiDeviceName[0])throw NativeFailure{{Outcome::refused,Step::output,uint32_t(error)}};
    return source.viewGdiDeviceName;
  }
  throw NativeFailure{{Outcome::handoffRequired,Step::output,ERROR_RETRY}};
}
}
bool validFrameShape(uint32_t width,uint32_t height,uint32_t pitch,size_t bytes){
  constexpr uint64_t maximumBytes=128ull*1024*1024;
  if(width<16||height<16||width>8192||height>8192||pitch<uint64_t(width)*4)return false;
  const uint64_t required=uint64_t(pitch)*(height-1)+uint64_t(width)*4;
  return required<=maximumBytes&&required<=bytes&&uint64_t(width)*height*4<=64ull*1024*1024;
}
Status acquireValidatedFrame(CaptureAccess& access,const std::function<Status()>& scope,BgraFrame& output,uint32_t timeoutMs){
  output={};if(!scope||timeoutMs>50)return {Outcome::refused,Step::acquire,0};
  bool held=false;
  try{
    auto status=scope();if(!status)return status;
    status=access.acquire(timeoutMs);if(!status)return status;
    held=true;BgraFrame candidate;
    status=access.read(candidate);
    if(status&&!validFrameShape(candidate.width,candidate.height,candidate.width*4,candidate.pixels.size()))status={Outcome::failed,Step::readback,0};
    if(status)status=scope();
    const auto released=access.release();held=false;
    if(!released)return released;
    if(!status)return status;
    output=std::move(candidate);return ready();
  }catch(const NativeFailure& failure){if(held)(void)access.release();return failure.status;}
  catch(...){if(held)(void)access.release();return {Outcome::failed,Step::readback,0};}
}
struct CaptureDxgi::Impl final:CaptureAccess {
  const Permit& permit;OwnedTarget target;DWORD thread=0;bool frameHeld=false,retired=false;
  uint64_t sequence=0;
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGIOutputDuplication> duplication;ComPtr<IDXGIResource> resource;
  ComPtr<ID3D11Texture2D> staging;D3D11_TEXTURE2D_DESC stageDescription{};
  explicit Impl(const Permit& value,const OwnedTarget& owned):permit(value),target(owned),thread(GetCurrentThreadId()){
    const auto scope=permit.current();if(!scope)throw NativeFailure{scope};
    if(permit.targets().size()!=1||permit.targets()[0]!=target||target.mask!=1||(!target.adapterLow&&!target.adapterHigh)||!validFrameShape(target.width,target.height,target.width*4,size_t(target.width)*target.height*4))
      throw NativeFailure{{Outcome::refused,Step::output,0}};
    const auto sourceName=ownedSourceName(target);
    ComPtr<IDXGIFactory1> factory;require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),Step::output);
    ComPtr<IDXGIAdapter1> selected;unsigned adapters=0;bool ended=false;
    for(UINT ordinal=0;ordinal<256;++ordinal){
      ComPtr<IDXGIAdapter1> candidate;const auto result=factory->EnumAdapters1(ordinal,&candidate);
      if(result==DXGI_ERROR_NOT_FOUND){ended=true;break;}require(result,Step::output);
      DXGI_ADAPTER_DESC1 description{};require(candidate->GetDesc1(&description),Step::output);
      if(same(description.AdapterLuid,target)){selected=candidate;++adapters;}
    }
    if(!ended||adapters!=1)throw NativeFailure{{Outcome::unsupported,Step::output,0}};
    ComPtr<IDXGIOutput1> output;unsigned matches=0;ended=false;
    for(UINT ordinal=0;ordinal<64;++ordinal){
      ComPtr<IDXGIOutput> candidate;const auto result=selected->EnumOutputs(ordinal,&candidate);
      if(result==DXGI_ERROR_NOT_FOUND){ended=true;break;}require(result,Step::output);
      DXGI_OUTPUT_DESC description{};require(candidate->GetDesc(&description),Step::output);
      if(_wcsicmp(description.DeviceName,sourceName.c_str())!=0)continue;
      MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
      if(!description.AttachedToDesktop||!description.Monitor||description.Rotation!=DXGI_MODE_ROTATION_IDENTITY||!equalRect(description.DesktopCoordinates,target)||
          !GetMonitorInfoW(description.Monitor,reinterpret_cast<MONITORINFO*>(&monitor))||_wcsicmp(monitor.szDevice,sourceName.c_str())!=0||!equalRect(monitor.rcMonitor,target))
        throw NativeFailure{{Outcome::refused,Step::output,0}};
      require(candidate.As(&output),Step::output);++matches;
    }
    if(!ended||matches!=1)throw NativeFailure{{Outcome::unsupported,Step::output,0}};
    // No WARP or arbitrary-adapter fallback: this exact adapter owns the output.
    require(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_SINGLETHREADED,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),Step::duplication);
    require(output->DuplicateOutput(device.Get(),&duplication),Step::duplication);
    DXGI_OUTDUPL_DESC description{};duplication->GetDesc(&description);
    if(description.ModeDesc.Width!=target.width||description.ModeDesc.Height!=target.height||description.ModeDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||description.Rotation!=DXGI_MODE_ROTATION_IDENTITY)
      throw NativeFailure{{Outcome::unsupported,Step::duplication,0}};
    const auto after=permit.current();if(!after)throw NativeFailure{after};
    // Ensure no path/mode/source change raced opening this exact output.
    if(ownedSourceName(target)!=sourceName)throw NativeFailure{{Outcome::handoffRequired,Step::output,ERROR_RETRY}};
  }
  ~Impl()override{if(frameHeld)(void)release();}
  Status acquire(uint32_t timeoutMs)override{
    if(retired||thread!=GetCurrentThreadId()||frameHeld)return {Outcome::handoffRequired,Step::acquire,0};
    DXGI_OUTDUPL_FRAME_INFO info{};const auto result=duplication->AcquireNextFrame(timeoutMs,&info,&resource);
    if(result==DXGI_ERROR_WAIT_TIMEOUT)return {Outcome::noFrame,Step::acquire,uint32_t(result)};
    if(FAILED(result)){retired=true;return hresult(Step::acquire,result);}
    frameHeld=true;
    // Even a malformed S_OK resource owns an acquired frame; read() will fail
    // and the shared orchestration must still call ReleaseFrame exactly once.
    return ready();
  }
  Status read(BgraFrame& frame)override{
    if(!frameHeld||!resource||thread!=GetCurrentThreadId())return {Outcome::refused,Step::texture,0};
    ComPtr<ID3D11Texture2D> texture;auto result=resource.As(&texture);if(FAILED(result))return hresult(Step::texture,result);
    D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);
    if(description.Width!=target.width||description.Height!=target.height||description.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||description.ArraySize!=1||description.SampleDesc.Count!=1)return {Outcome::handoffRequired,Step::texture,0};
    if(!staging){
      stageDescription=description;stageDescription.MipLevels=1;stageDescription.ArraySize=1;stageDescription.Usage=D3D11_USAGE_STAGING;
      stageDescription.BindFlags=0;stageDescription.CPUAccessFlags=D3D11_CPU_ACCESS_READ;stageDescription.MiscFlags=0;
      result=device->CreateTexture2D(&stageDescription,nullptr,&staging);if(FAILED(result))return hresult(Step::readback,result);
    }
    context->CopyResource(staging.Get(),texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};result=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    if(FAILED(result))return hresult(Step::readback,result);
    struct Unmap {ID3D11DeviceContext* context;ID3D11Texture2D* texture;~Unmap(){context->Unmap(texture,0);}}unmap{context.Get(),staging.Get()};
    const uint64_t mappedBytes=uint64_t(mapped.RowPitch)*target.height;
    if(!mapped.pData||mappedBytes>128ull*1024*1024||!validFrameShape(target.width,target.height,mapped.RowPitch,size_t(mappedBytes)))return {Outcome::refused,Step::readback,0};
    frame.width=target.width;frame.height=target.height;frame.pixels.resize(size_t(target.width)*target.height*4);
    const auto* pixels=static_cast<const uint8_t*>(mapped.pData);
    for(uint32_t y=0;y<target.height;++y)std::memcpy(frame.pixels.data()+size_t(y)*target.width*4,pixels+size_t(y)*mapped.RowPitch,size_t(target.width)*4);
    return ready();
  }
  Status release()noexcept override{
    if(!frameHeld)return {Outcome::failed,Step::release,0};
    resource.Reset();frameHeld=false;const auto result=duplication->ReleaseFrame();
    if(FAILED(result)){retired=true;return hresult(Step::release,result);}return ready();
  }
};
CaptureDxgi::CaptureDxgi(const Permit& permit,const OwnedTarget& target):impl_(std::make_unique<Impl>(permit,target)){}
CaptureDxgi::~CaptureDxgi()=default;
Status CaptureDxgi::next(BgraFrame& frame,uint32_t timeoutMs){
  if(!impl_){frame={};return {Outcome::refused,Step::acquire,0};}
  auto status=acquireValidatedFrame(*impl_,[&]{return impl_->permit.current();},frame,timeoutMs);
  if(status){if(impl_->sequence==UINT64_MAX){frame={};impl_->retired=true;return {Outcome::handoffRequired,Step::acquire,0};}frame.sequence=++impl_->sequence;}
  else if(status.outcome==Outcome::handoffRequired||status.outcome==Outcome::refused)impl_->retired=true;
  return status;
}
}
