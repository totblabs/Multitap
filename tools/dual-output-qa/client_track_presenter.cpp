#include "remote_track_presenter.h"
#include "remote_window_group.h"
#include "../../native/windows_video/color.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace multitap {
struct RemoteTrackPresenterTestPeer {
  static bool ExactPixels(RemoteTrackPresenter& presenter,const DecodedFrame& frame){
    if(!presenter.lastBuffer_||!presenter.device_||!presenter.context_)return false;
    D3D11_TEXTURE2D_DESC desc{};presenter.lastBuffer_->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if(FAILED(presenter.device_->CreateTexture2D(&desc,nullptr,&staging)))return false;
    presenter.context_->CopyResource(staging.Get(),presenter.lastBuffer_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(presenter.context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
    bool same=true;for(uint32_t y=0;y<frame.height&&same;++y)
      same=std::memcmp(static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,frame.rgba.data()+size_t(y)*frame.width*4,size_t(frame.width)*4)==0;
    presenter.context_->Unmap(staging.Get(),0);return same;
  }
  // The back buffer of a presenter on a track engine's device (BGRA) against RGBA pixels.
  static bool ExactBgra(RemoteTrackPresenter& presenter,const std::vector<uint8_t>& rgba,uint32_t width,uint32_t height){
    if(!presenter.lastBuffer_||!presenter.device_||!presenter.context_||presenter.format_!=DXGI_FORMAT_B8G8R8A8_UNORM)return false;
    D3D11_TEXTURE2D_DESC desc{};presenter.lastBuffer_->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if(FAILED(presenter.device_->CreateTexture2D(&desc,nullptr,&staging)))return false;
    presenter.context_->CopyResource(staging.Get(),presenter.lastBuffer_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(presenter.context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
    bool same=desc.Width==width&&desc.Height==height;
    for(uint32_t y=0;y<height&&same;++y)for(uint32_t x=0;x<width&&same;++x){
      const uint8_t* b=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;const uint8_t* r=&rgba[(size_t(y)*width+x)*4];
      same=b[0]==r[2]&&b[1]==r[1]&&b[2]==r[0]&&b[3]==r[3];
    }
    presenter.context_->Unmap(staging.Get(),0);return same;
  }
  static void FailGpu(RemoteTrackPresenter& presenter){presenter.failGpuForTest_=true;}
};
}
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Window {
  HWND value=nullptr;
  Window(int x,int y,int width,int height,HWND parent=nullptr,bool child=false){
    value=CreateWindowExW(WS_EX_TOOLWINDOW,L"Multitap.TrackPresenter.HiddenQA",L"Multitap hidden output QA",child?WS_CHILD:WS_POPUP,x,y,width,height,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(value!=nullptr,"Owned hidden QA window could not be created");
  }
  ~Window(){if(IsWindow(value))DestroyWindow(value);}
};
multitap::DecodedFrame frame(uint32_t width,uint32_t height,unsigned seed){
  multitap::DecodedFrame value;value.width=width;value.height=height;value.rgba.resize(size_t(width)*height*4);
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){const auto at=(size_t(y)*width+x)*4;
    value.rgba[at]=uint8_t(x*13+y*7+seed);value.rgba[at+1]=uint8_t(x*5+y*11+seed*3);value.rgba[at+2]=uint8_t(x*17+y*3+seed*5);value.rgba[at+3]=255;}
  return value;
}
POINT screen(HWND window,LONG x,LONG y){POINT value{x,y};require(ClientToScreen(window,&value)!=FALSE,"QA client coordinate conversion failed");return value;}
// A GPU picture made by the track engine's converter on a WARP device, with the
// CPU converter's pixels as the reference.
struct GpuPicture {multitap::DecodedFrame frame;std::vector<uint8_t> reference;};
GpuPicture gpuPicture(const std::shared_ptr<multitap::gpu::Device>& device,uint32_t width,uint32_t height,unsigned seed){
  std::vector<uint8_t> nv12(size_t(width)*height*3/2);
  for(size_t i=0;i<nv12.size();++i)nv12[i]=uint8_t(16+(i*7+seed*13)%220);
  GpuPicture picture;picture.reference.resize(size_t(width)*height*4);
  multitap::video::nv12ToRgba(nv12.data(),picture.reference.data(),width,height,width,height,false,false);
  multitap::gpu::Converter converter(device);
  picture.frame.width=width;picture.frame.height=height;
  require(SUCCEEDED(converter.FromSystem(nv12.data(),width,height,width,height,width,height,multitap::gpu::Colour{},picture.frame.gpu))&&picture.frame.gpu,"GPU picture could not be made");
  return picture;
}
void render(multitap::RemoteTrackPresenter& presenter,const multitap::DecodedFrame& value){
  require(presenter.Upload(value)==S_OK,"Actual D3D11 source upload failed");
  require(multitap::RemoteTrackPresenterTestPeer::ExactPixels(presenter,value),"Actual D3D11 buffer pixels differed from the selected output");
  const HRESULT hr=presenter.Present();
  require(SUCCEEDED(hr)||hr==DXGI_ERROR_WAS_STILL_DRAWING,"Actual D3D11 upload/present failed");
}
}
int main(){try{
  const auto previousDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSW windowClass{};windowClass.lpfnWndProc=DefWindowProcW;windowClass.hInstance=GetModuleHandleW(nullptr);windowClass.lpszClassName=L"Multitap.TrackPresenter.HiddenQA";
  require(RegisterClassW(&windowClass)!=0,"Owned hidden QA class registration failed");
  uint64_t uploads=0,accepted=0,occluded=0,busy=0;
  {
    Window first(-2200,20,640,360),second(-1200,500,800,600),dialog(10,10,200,160,first.value),child(0,0,128,128,first.value,true);
    multitap::RemoteTrackPresenter left,right;
    require(SUCCEEDED(left.Initialize(first.value,true))&&SUCCEEDED(right.Initialize(second.value,true)),"Two independent actual WARP presenters could not initialize");
    require(left.stats().warp&&right.stats().warp,"QA did not use the requested real software D3D11 device");
    auto a=frame(640,360,17),b=frame(800,600,91);
    require(left.Upload(a)==S_OK&&right.Upload(b)==S_OK,"Parallel back-buffer uploads failed");
    require(multitap::RemoteTrackPresenterTestPeer::ExactPixels(left,a),"Second output upload changed the first output buffer");
    require(multitap::RemoteTrackPresenterTestPeer::ExactPixels(right,b),"Second output did not receive its own pixels");
    const auto before=right.stats().uploads;
    auto wrong=frame(640,360,33);require(right.Render(wrong)==S_FALSE&&right.stats().uploads==before,"Mismatched source was stretched or cropped into the viewport");
    require(multitap::RemoteTrackPresenterTestPeer::ExactPixels(right,b),"Pending viewport replaced valid output pixels");
    render(right,b);
    wrong=b;wrong.rgba.pop_back();require(right.Render(wrong)==E_INVALIDARG,"Truncated frame was accepted");
    require(SetWindowPos(second.value,nullptr,-1200,500,960,540,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Owned QA window resize failed");
    b=frame(960,540,44);render(right,b);require(multitap::RemoteTrackPresenterTestPeer::ExactPixels(left,a),"Output resize changed the other output");
    render(left,a);
    uploads=left.stats().uploads+right.stats().uploads;accepted=left.stats().presentAccepted+right.stats().presentAccepted;occluded=left.stats().occluded+right.stats().occluded;busy=left.stats().busy+right.stats().busy;
    for(unsigned i=0;i<3;++i){right.Close();require(SUCCEEDED(right.Initialize(second.value,true)),"Retired flip chain prevented same-window replacement");render(right,b);}
    using Group=multitap::RemoteWindowGroup;Group group;
    const Group::Surface sa{0,3,640,360,first.value,{0,0,640,360},{{0,0,80,40}}};
    const Group::Surface sb{1,7,960,540,second.value,{0,0,960,540},{}};
    require(group.Configure(9,{sa,sb}),"Unequal hidden output geometry was rejected");
    require(group.Contains(first.value)&&group.Contains(child.value)&&group.Contains(second.value)&&!group.Contains(dialog.value),"Dialog or non-group input ownership was incorrect");
    require(!group.Map(screen(first.value,10,10)),"Local toolbar exclusion mapped to remote input");
    const auto p=group.Map(screen(first.value,639,359));require(p&&p->track==0&&p->epoch==3&&p->topology==9&&p->x==1.0&&p->y==1.0,"First output bottom edge mapping was incorrect");
    const auto q=group.Map(screen(second.value,0,0));require(q&&q->track==1&&q->epoch==7&&q->x==0&&q->y==0,"Second output start mapping was incorrect");
    require(!group.Map(screen(first.value,640,360))&&!group.Map(POINT{-1500,450}),"Outside pixels or the gap between outputs were mapped");
    require(!group.MapVisible(screen(first.value,100,100)),"A hidden QA output was accepted as a visible input target");
    auto invalid=sb;invalid.window=first.value;invalid.width=640;invalid.height=360;invalid.image={0,0,640,360};
    require(!group.Configure(10,{sa,invalid})&&group.topology()==9,"Duplicate local roots replaced a valid group");
    invalid=sb;invalid.track=0;require(!group.Configure(10,{sa,invalid}),"Duplicate remote track IDs were accepted");
    invalid=sb;invalid.epoch=0;require(!group.Configure(10,{sa,invalid}),"Unscoped output epoch was accepted");
    invalid=sb;invalid.image.right=959;require(!group.Configure(10,{sa,invalid}),"Stretched image geometry was accepted");
    require(SetWindowPos(second.value,nullptr,-2200,20,640,360,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Owned overlap fixture move failed");
    require(!group.Map(screen(first.value,100,100)),"A shrunken second viewport remained mapped");
    invalid=sb;invalid.width=640;invalid.height=360;invalid.image={0,0,640,360};
    require(!group.Configure(10,{sa,invalid}),"Overlapping local output rectangles were accepted");
    require(SetWindowPos(second.value,nullptr,-1200,500,960,540,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Owned QA viewport restore failed");
    require(group.Configure(11,{sb}),"Sparse output1-only group was rejected");
    const auto sparse=group.Map(screen(second.value,959,539));require(sparse&&sparse->track==1&&sparse->topology==11&&sparse->x==1.0&&sparse->y==1.0,"Sparse output1-only map retained a prior group");
    group.Clear();require(!group.Contains(first.value)&&!group.Map(screen(second.value,10,10)),"Closed output group retained input geometry");
  }
  // GPU pictures: the swap chain lives on the picture's own device and the pixels
  // are copied GPU to GPU; a failure presents the same pixels through the CPU.
  uint64_t gpuUploads=0,gpuFallbacks=0;
  {
    const auto deviceA=multitap::gpu::Device::Create(nullptr,true),deviceB=multitap::gpu::Device::Create(nullptr,true);
    require(deviceA&&deviceB&&deviceA!=deviceB,"Two WARP devices expected");
    Window first(-2200,20,640,360),second(-1200,500,960,540);
    multitap::RemoteTrackPresenter left,right;
    require(SUCCEEDED(left.Initialize(first.value,true))&&SUCCEEDED(right.Initialize(second.value,true)),"Presenters for GPU pictures could not initialize");
    require(!left.stats().gpu,"A presenter started on a track device before any GPU picture");
    auto a=gpuPicture(deviceA,640,360,3);
    require(left.Upload(a.frame)==S_OK&&left.stats().gpu&&left.stats().gpuUploads==1&&left.stats().gpuFallbacks==0,"GPU picture was not copied GPU to GPU");
    require(multitap::RemoteTrackPresenterTestPeer::ExactBgra(left,a.reference,640,360),"GPU copy differs from the CPU converter's pixels");
    (void)left.Present();
    auto wrong=gpuPicture(deviceA,320,180,5);const auto before=left.stats().uploads;
    require(left.Render(wrong.frame)==S_FALSE&&left.stats().uploads==before,"Mismatched GPU picture was stretched or cropped into the viewport");
    // A CPU picture on a presenter that lives on a track device is swizzled, not rejected.
    auto cpu=frame(640,360,29);
    require(left.Upload(cpu)==S_OK&&left.stats().gpu&&multitap::RemoteTrackPresenterTestPeer::ExactBgra(left,cpu.rgba,640,360),"CPU picture on a track-device presenter differs");
    // Another device (another adapter in production): the swap chain follows the picture.
    auto b=gpuPicture(deviceB,960,540,9);
    require(right.Upload(b.frame)==S_OK&&right.stats().gpu&&multitap::RemoteTrackPresenterTestPeer::ExactBgra(right,b.reference,960,540),"GPU picture on the second device differs");
    auto c=gpuPicture(deviceA,960,540,11);
    require(right.Upload(c.frame)==S_OK&&multitap::RemoteTrackPresenterTestPeer::ExactBgra(right,c.reference,960,540),"Swap chain did not follow the picture's device");
    const HRESULT presented=right.Present();require(SUCCEEDED(presented)||presented==DXGI_ERROR_WAS_STILL_DRAWING,"GPU picture was not presented");
    require(SetWindowPos(second.value,nullptr,-1200,500,640,360,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Owned QA window resize failed");
    auto d=gpuPicture(deviceA,640,360,15);
    require(right.Upload(d.frame)==S_OK&&multitap::RemoteTrackPresenterTestPeer::ExactBgra(right,d.reference,640,360),"GPU picture after a viewport resize differs");
    // Any GPU failure: the same pixels through a CPU readback, and it stays on the CPU.
    multitap::RemoteTrackPresenterTestPeer::FailGpu(left);
    auto e=gpuPicture(deviceA,640,360,21);
    require(left.Upload(e.frame)==S_OK&&!left.stats().gpu&&left.stats().gpuFallbacks==1&&left.stats().gpuFailure==E_FAIL&&SUCCEEDED(left.stats().failure),"GPU failure did not fall back to the CPU");
    require(multitap::RemoteTrackPresenterTestPeer::ExactPixels(left,[&]{multitap::DecodedFrame f;f.width=640;f.height=360;f.rgba=e.reference;return f;}()),"CPU fallback differs from the CPU converter's pixels");
    auto f=gpuPicture(deviceA,640,360,23);
    require(left.Upload(f.frame)==S_OK&&left.stats().gpuFallbacks==2&&left.stats().gpuUploads==1,"A presenter that gave up GPU presentation tried it again");
    const HRESULT again=left.Present();require(SUCCEEDED(again)||again==DXGI_ERROR_WAS_STILL_DRAWING,"CPU fallback was not presented");
    gpuUploads=left.stats().gpuUploads+right.stats().gpuUploads;gpuFallbacks=left.stats().gpuFallbacks+right.stats().gpuFallbacks;
    left.Close();right.Close();
  }
  MSG message{};require(!PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_NOREMOVE),"Owned output cleanup posted application quit");
  UnregisterClassW(windowClass.lpszClassName,windowClass.hInstance);if(previousDpi)SetThreadDpiAwarenessContext(previousDpi);
  std::cout<<"{\"kind\":\"actual-hidden-track-presenter\",\"checkpoints\":"<<checks<<",\"independentD3d11WarpDevices\":2,\"initialSequenceUploads\":"<<uploads<<",\"initialPresentAccepted\":"<<accepted<<",\"initialOccluded\":"<<occluded<<",\"initialBusy\":"<<busy<<",\"sameWindowReplacements\":3,\"gpuPictureUploads\":"<<gpuUploads<<",\"gpuFallbackUploads\":"<<gpuFallbacks<<",\"gpuPixelsExact\":true,\"exactGpuBufferPixelsVerified\":true,\"physicalPresentation\":false,\"keyboardCapture\":false,\"windowsDisplayModesChanged\":false,\"realVmAcceptance\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
