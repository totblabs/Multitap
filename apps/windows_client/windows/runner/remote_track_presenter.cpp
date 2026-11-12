#include "remote_track_presenter.h"
#include "video_bounds.h"
#include <chrono>

namespace multitap {
using Microsoft::WRL::ComPtr;
void RemoteTrackPresenter::Release(){
  lastBuffer_.Reset();swap_.Reset();
  // A private device is ours to reset. A track engine's device is shared with
  // its decode/convert worker: clearing its state would break a draw in flight.
  if(context_){if(!shared_)context_->ClearState();context_->Flush();}
  context_.Reset();device_.Reset();shared_.reset();width_=height_=0;uploaded_=false;
}
void RemoteTrackPresenter::Close(){Release();window_=nullptr;gpuGivenUp_=false;}
HRESULT RemoteTrackPresenter::Build(std::shared_ptr<gpu::Device> shared){
  Release();
  HRESULT hr=S_OK;
  if(shared){
    device_=shared->device;context_=shared->context;shared_=std::move(shared);format_=DXGI_FORMAT_B8G8R8A8_UNORM;stats_.warp=shared_->info.software;
  }else{
    format_=DXGI_FORMAT_R8G8B8A8_UNORM;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    hr=D3D11CreateDevice(nullptr,forceWarp_?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,3,D3D11_SDK_VERSION,&device_,nullptr,&context_);
    stats_.warp=forceWarp_;
    if(FAILED(hr)&&!forceWarp_){device_.Reset();context_.Reset();hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,3,D3D11_SDK_VERSION,&device_,nullptr,&context_);stats_.warp=true;}
  }
  stats_.gpu=shared_!=nullptr;
  if(FAILED(hr)){Release();return hr;}
  ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
  hr=device_.As(&dxgi);if(SUCCEEDED(hr))hr=dxgi->GetAdapter(&adapter);if(SUCCEEDED(hr))hr=adapter->GetParent(IID_PPV_ARGS(&factory));
  RECT rect{};if(SUCCEEDED(hr)&&!GetClientRect(window_,&rect))hr=HRESULT_FROM_WIN32(GetLastError());
  const auto width=rect.right-rect.left,height=rect.bottom-rect.top;
  if(SUCCEEDED(hr)&&!ValidVideoSize(width,height))hr=E_INVALIDARG;
  if(SUCCEEDED(hr)){
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=uint32_t(width);desc.Height=uint32_t(height);desc.Format=format_;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.Scaling=DXGI_SCALING_NONE;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    hr=factory->CreateSwapChainForHwnd(device_.Get(),window_,&desc,nullptr,nullptr,&swap_);
    if(SUCCEEDED(hr)){factory->MakeWindowAssociation(window_,DXGI_MWA_NO_ALT_ENTER);width_=uint32_t(width);height_=uint32_t(height);}
  }
  if(FAILED(hr))Release();
  return hr;
}
HRESULT RemoteTrackPresenter::Initialize(HWND window,bool forceWarp){
  if(!window||!IsWindow(window))return E_INVALIDARG;
  Close();stats_={};window_=window;forceWarp_=forceWarp;
  const HRESULT hr=Build(nullptr);
  if(FAILED(hr)){stats_.failure=hr;Close();}
  return hr;
}
HRESULT RemoteTrackPresenter::Resize(uint32_t width,uint32_t height){
  if(width_==width&&height_==height)return S_OK;
  lastBuffer_.Reset();if(!shared_)context_->ClearState();
  const HRESULT hr=swap_->ResizeBuffers(2,width,height,format_,0);
  if(SUCCEEDED(hr)){width_=width;height_=height;}return hr;
}
HRESULT RemoteTrackPresenter::UploadCpu(const uint8_t* rgba,uint32_t width,uint32_t height){
  // The swap chain follows the pixels' owner: a shared (GPU picture) device
  // keeps its BGRA chain, so CPU pixels are swizzled; otherwise RGBA as-is.
  if((shared_&&gpuGivenUp_)||!swap_){
    // Back on a private device for good (or after a failed rebuild).
    const HRESULT built=Build(nullptr);if(FAILED(built))return built;
  }
  HRESULT hr=Resize(width,height);if(FAILED(hr))return hr;
  lastBuffer_.Reset();hr=swap_->GetBuffer(0,IID_PPV_ARGS(&lastBuffer_));if(FAILED(hr))return hr;
  const uint8_t* source=rgba;
  if(format_==DXGI_FORMAT_B8G8R8A8_UNORM){
    scratch_.resize(size_t(width)*height*4);
    for(size_t i=0;i<scratch_.size();i+=4){scratch_[i]=rgba[i+2];scratch_[i+1]=rgba[i+1];scratch_[i+2]=rgba[i];scratch_[i+3]=rgba[i+3];}
    source=scratch_.data();
  }
  context_->UpdateSubresource(lastBuffer_.Get(),0,nullptr,source,width*4,0);
  return S_OK;
}
HRESULT RemoteTrackPresenter::UploadGpu(const DecodedFrame& frame){
  if(failGpuForTest_)return E_FAIL;  // tests only: exercises the CPU fallback
  const auto& picture=*frame.gpu;
  if(!picture.owner||!picture.owner->device||!picture.texture||picture.width!=frame.width||picture.height!=frame.height)return E_INVALIDARG;
  if(shared_!=picture.owner){const HRESULT hr=Build(picture.owner);if(FAILED(hr))return hr;}
  HRESULT hr=Resize(frame.width,frame.height);if(FAILED(hr))return hr;
  lastBuffer_.Reset();hr=swap_->GetBuffer(0,IID_PPV_ARGS(&lastBuffer_));if(FAILED(hr))return hr;
  // The converter finished its GPU work before the picture was published; both
  // run on this device's immediate context, so the copy sees the finished pixels.
  context_->CopyResource(lastBuffer_.Get(),picture.texture.Get());
  return S_OK;
}
HRESULT RemoteTrackPresenter::Upload(const DecodedFrame& frame){
  if(FAILED(stats_.failure))return stats_.failure;
  const bool gpuPicture=frame.gpu!=nullptr;
  if(!window_||!IsWindow(window_)||!ValidVideoSize(frame.width,frame.height)||(gpuPicture?!frame.rgba.empty():frame.rgba.size()!=size_t(frame.width)*frame.height*4))return E_INVALIDARG;
  RECT rect{};if(!GetClientRect(window_,&rect))return HRESULT_FROM_WIN32(GetLastError());
  // Only fresh matching source pixels can establish a viewport. Do not crop,
  // stretch or center a smaller image and then report an applied resolution.
  if(rect.right-rect.left!=int64_t(frame.width)||rect.bottom-rect.top!=int64_t(frame.height)){++stats_.viewportPending;return S_FALSE;}
  const auto start=std::chrono::steady_clock::now();
  HRESULT hr=S_OK;
  if(gpuPicture){
    if(!gpuGivenUp_){
      hr=UploadGpu(frame);
      if(SUCCEEDED(hr))++stats_.gpuUploads;
      else{stats_.gpuFailure=hr;gpuGivenUp_=true;}
    }
    if(gpuGivenUp_){
      // Same pixels through the CPU: a readback of the GPU picture.
      std::vector<uint8_t> rgba;
      hr=frame.gpu->ReadRgba(rgba)&&rgba.size()==size_t(frame.width)*frame.height*4?UploadCpu(rgba.data(),frame.width,frame.height):E_FAIL;
      if(SUCCEEDED(hr))++stats_.gpuFallbacks;
    }
  }else hr=UploadCpu(frame.rgba.data(),frame.width,frame.height);
  if(FAILED(hr)){stats_.failure=hr;return hr;}
  ++stats_.uploads;
  stats_.uploadTotalMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  uploaded_=true;return S_OK;
}
HRESULT RemoteTrackPresenter::Present(){
  if(FAILED(stats_.failure))return stats_.failure;
  if(!window_||!IsWindow(window_)||!swap_||!uploaded_)return E_UNEXPECTED;
  const auto presenting=std::chrono::steady_clock::now();++stats_.presentCalls;
  const HRESULT hr=swap_->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
  stats_.presentTotalMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-presenting).count();
  if(hr==DXGI_ERROR_WAS_STILL_DRAWING){++stats_.busy;return hr;}
  // FLIP_DISCARD does not preserve back-buffer contents after presentation.
  // Only a fresh upload may be presented again, including after occlusion.
  uploaded_=false;
  if(hr==DXGI_STATUS_OCCLUDED){++stats_.occluded;return hr;}
  if(hr==S_OK)++stats_.presentAccepted;else if(FAILED(hr))stats_.failure=hr;
  return hr;
}
HRESULT RemoteTrackPresenter::Render(const DecodedFrame& frame){const HRESULT hr=Upload(frame);return hr==S_OK?Present():hr;}
}
