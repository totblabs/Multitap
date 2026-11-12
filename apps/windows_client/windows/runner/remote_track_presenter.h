#pragma once
#include "remote_decoder.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>

namespace multitap {
// UI-thread uploader for one owned native output window. This never changes a
// Windows display mode or treats an image stretch as a resize.
//
// CPU pictures (DecodedFrame::rgba) are uploaded into the swap chain's back
// buffer. A GPU picture (DecodedFrame::gpu, the track engine's zero-copy path) is
// copied GPU to GPU from its shared texture: the swap chain is created on the
// device that owns the picture, i.e. on the adapter the track's decoder and
// converter were chosen for (the adapter driving this window's monitor when it
// decodes H.264), so no pixel crosses the CPU. Any failure of that path - the
// device cannot present to this window, a copy that cannot be set up - presents
// the same picture through a CPU readback instead and keeps doing so (stats
// gpuFallbacks / gpuFailure); the window never goes blank because of it.
class RemoteTrackPresenter {
 public:
  struct Stats {
    uint64_t uploads=0,presentCalls=0,presentAccepted=0,occluded=0,busy=0,viewportPending=0;
    // Uploads that were GPU copies, pictures that fell back to a CPU readback.
    uint64_t gpuUploads=0,gpuFallbacks=0;
    double uploadTotalMs=0,presentTotalMs=0;
    bool warp=false;
    // The swap chain currently lives on a track engine's (GPU picture) device.
    bool gpu=false;
    HRESULT failure=S_OK;
    // Why GPU presentation was given up (S_OK: not).
    HRESULT gpuFailure=S_OK;
  };
  HRESULT Initialize(HWND window,bool forceWarp=false);
  HRESULT Upload(const DecodedFrame& frame);
  HRESULT Present();
  HRESULT Render(const DecodedFrame& frame);
  void Close();
  const Stats& stats()const{return stats_;}
 private:
  friend struct RemoteTrackPresenterTestPeer;
  // Creates the swap chain on `shared`'s device (GPU pictures) or on a private
  // hardware/WARP device (CPU pictures).
  HRESULT Build(std::shared_ptr<gpu::Device> shared);
  void Release();
  HRESULT Resize(uint32_t width,uint32_t height);
  HRESULT UploadGpu(const DecodedFrame& frame);
  HRESULT UploadCpu(const uint8_t* rgba,uint32_t width,uint32_t height);
  HWND window_=nullptr;
  bool forceWarp_=false,gpuGivenUp_=false,failGpuForTest_=false;
  std::shared_ptr<gpu::Device> shared_;
  DXGI_FORMAT format_=DXGI_FORMAT_R8G8B8A8_UNORM;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> lastBuffer_;
  std::vector<uint8_t> scratch_;
  uint32_t width_=0,height_=0;
  bool uploaded_=false;
  Stats stats_;
};
}
