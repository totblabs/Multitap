#pragma once
#include "stream_size_admission.h"
#include "remote_decoder.h"
#include "video_memory.h"
namespace multitap {
struct NativeStreamSizeResult {
  const char* reason=nullptr;HRESULT hresult=S_OK;
  const char* decoderStage="not_queried";
  std::array<bool,2> hardware{};StreamMemory required{};
  std::array<bool,2> accepted{};
};
// Calls only exact metadata/type admission. No packets, frame decode, input,
// services, driver configuration, or explicit frame/texture buffer allocation.
// Allocation internal to the isolated decoder/driver is not observed here.
inline NativeStreamSizeResult ProbeNativeStreamSizesOnWorker(const StreamSize* shapes,size_t count,bool hardware,IDXGIAdapter* render,
    const std::array<Microsoft::WRL::ComPtr<IDXGIAdapter>,2>& monitors={}){
  NativeStreamSizeResult answer;
  if(!StreamMemoryNeeded(shapes,count,answer.required)){answer.reason="stream_size_abi";answer.hresult=E_INVALIDARG;return answer;}
  const auto available=AvailableVideoSystemMemory();
  if(!available||answer.required.systemBytes>available){answer.reason=available?"stream_memory_budget":"stream_memory_unknown";answer.hresult=E_OUTOFMEMORY;return answer;}
  try{
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com)){answer.reason="stream_decoder_unavailable";answer.hresult=com;return answer;}
    const auto mf=MFStartup(MF_VERSION,MFSTARTUP_LITE);
    if(FAILED(mf)){answer.reason="stream_decoder_unavailable";answer.hresult=mf;CoUninitialize();return answer;}
    try{
      // Retain each selected device through the whole aggregate check.
      std::array<gpu::Selection,2> selections{};
      const auto presenter=render?gpu::Device::Create(render,false,nullptr,false):nullptr;
      uint64_t presenterBytes=0;
      if(!presenter){answer.reason="stream_texture_unknown";answer.hresult=E_NOTIMPL;}
      for(size_t i=0;!answer.reason&&i<count;++i){
        const auto& s=shapes[i];const auto width=s.sourceWidth?s.sourceWidth:s.width,height=s.sourceHeight?s.sourceHeight:s.height;
        video::FrameLayout source{};
        if(!video::frameLayout(width,height,source,false)||!video::checkedAdd(presenterBytes,uint64_t(source.rgbaBytes)*2,presenterBytes)){answer.reason="stream_size_abi";answer.hresult=E_INVALIDARG;break;}
        if(width>presenter->TextureDimensionLimit()||height>presenter->TextureDimensionLimit()){answer.reason="stream_texture_dimension";answer.hresult=E_INVALIDARG;break;}
      }
      if(!answer.reason){const auto availableGpu=presenter->AvailableTextureMemory();if(!availableGpu||presenterBytes>availableGpu){answer.reason=availableGpu?"stream_texture_budget":"stream_texture_unknown";answer.hresult=E_OUTOFMEMORY;}}
      if(!answer.reason&&presenter->info.software){uint64_t combined=0;if(!video::checkedAdd(answer.required.systemBytes,presenterBytes,combined)||combined>AvailableVideoSystemMemory()){answer.reason="stream_memory_budget";answer.hresult=E_OUTOFMEMORY;}}
      for(size_t i=0;i<count;++i){
        if(answer.reason)break;
        if(answer.required.systemBytes>AvailableVideoSystemMemory()){answer.reason="stream_memory_budget";answer.hresult=E_OUTOFMEMORY;break;}
        const auto& s=shapes[i];RemoteDecoder::Options options;options.fps=s.fps;
        HRESULT hr=E_FAIL;
        if(hardware){
          gpu::SelectionOptions choice;choice.hardware_decode=choice.gpu_present=true;choice.isolated_query=true;
          selections[i]=gpu::SelectForTrack(monitors[i].Get(),render,s.width,s.height,choice);
          uint64_t allDecodeBytes=0;for(size_t j=0;j<count;++j){VideoLayout layout{};if(!CheckedVideoLayout(shapes[j].width,shapes[j].height,layout)||!video::checkedAdd(allDecodeBytes,uint64_t(layout.nv12Bytes)*18,allDecodeBytes)){answer.reason="stream_size_abi";answer.hresult=E_INVALIDARG;break;}}
          const bool shared=selections[i].render&&selections[i].decode&&gpu::SameLuid(selections[i].render->info.luid,selections[i].decode->info.luid);
          if(!answer.reason&&selections[i].render&&!selections[i].render->TextureFits(s.sourceWidth?s.sourceWidth:s.width,s.sourceHeight?s.sourceHeight:s.height,answer.required.gpuBytes+(shared?allDecodeBytes:0)))selections[i].render.reset();
          if(selections[i].decode){
            // Conservatively budget both tracks on this actual selected device;
            // never allow each track to spend the same free budget independently.
            const uint64_t need=allDecodeBytes+(selections[i].render&&shared?answer.required.gpuBytes:0);
            if(!answer.reason&&selections[i].decode->DecodeFits(s.width,s.height,need)){
              options.decode_device=selections[i].decode;hr=RemoteDecoder::ValidateShape(s.width,s.height,s.fps,options,&answer.decoderStage);
              if(SUCCEEDED(hr))answer.hardware[i]=true;
            }
          }
        }
        if(answer.reason)break;
        // Same selected Microsoft MFT; CPU is attempted when the selected DXVA
        // path is unavailable, fails type admission or cannot fit its budget.
        if(!answer.hardware[i]){options.decode_device.reset();hr=RemoteDecoder::ValidateShape(s.width,s.height,s.fps,options,&answer.decoderStage);}
        if(FAILED(hr)){answer.reason="stream_decoder_type";answer.hresult=hr;break;}
        answer.accepted[i]=true;
      }
    }catch(const std::bad_alloc&){answer.reason="stream_memory_budget";answer.hresult=E_OUTOFMEMORY;}
    catch(...){answer.reason="stream_decoder_unavailable";answer.hresult=E_FAIL;}
    MFShutdown();CoUninitialize();
  }catch(...){answer.reason="stream_decoder_unavailable";answer.hresult=E_FAIL;}
  return answer;
}
// Standalone passive CLI only. The Flutter bridge always submits the worker
// entry through the single process-owned asynchronous slot; never call on UI.
inline NativeStreamSizeResult ProbeNativeStreamSizes(const StreamSize* shapes,size_t count,bool hardware,IDXGIAdapter* render,
    const std::array<Microsoft::WRL::ComPtr<IDXGIAdapter>,2>& monitors={}){
  return ProbeNativeStreamSizesOnWorker(shapes,count,hardware,render,monitors);
}
}
