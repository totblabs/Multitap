#include <stream_size_native.h>
#include <cstdio>
#include <cstring>
int main(int argc,char** argv){
  if(argc!=2||std::strcmp(argv[1],"--root-passive-type-only")){std::fprintf(stderr,"Root review and --root-passive-type-only required.\n");return 2;}
  using namespace multitap;using Microsoft::WRL::ComPtr;
  // This standalone adapter is enumerated, NOT proven to be the adapter of a
  // running Flutter window. The app uses its actual view's graphics adapter.
  ComPtr<IDXGIFactory1> factory;ComPtr<IDXGIAdapter1> adapter;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||FAILED(factory->EnumAdapters1(0,&adapter))){std::puts("{\"stage\":\"adapter-query\",\"unknown\":true,\"framesDecoded\":0}");return 2;}
  struct Case {const char* name;std::array<StreamSize,2> sizes;size_t count;};
  const Case cases[]={
    {"1080p",{{{1920,1080,60},{}}},1},{"4K",{{{3840,2160,60},{}}},1},
    {"5120x2880",{{{5120,2880,60},{}}},1},{"5120x1440",{{{5120,1440,60},{}}},1},
    {"2880x5120",{{{2880,5120,60},{}}},1},{"dual-2x4K",{{{3840,2160,60},{3840,2160,60}}},2}
  };
  for(const auto& item:cases)for(const bool hardware:{false,true}){
    StreamMemory required{};const auto available=AvailableVideoSystemMemory();
    if(!StreamMemoryNeeded(item.sizes.data(),item.count,required)||!available||required.systemBytes>available){std::puts("{\"stage\":\"memory-preflight\",\"stopped\":true,\"framesDecoded\":0}");return 2;}
    const auto result=ProbeNativeStreamSizes(item.sizes.data(),item.count,hardware,adapter.Get());
    std::printf("{\"case\":\"%s\",\"hardwareRequested\":%s,\"accepted\":%s,\"reason\":\"%s\",\"hresult\":%ld,\"decoderStage\":\"%s\",\"tracks\":%zu,\"inputAndNv12TypesNegotiated\":%s,\"inputTypeTestOnly\":false,\"framesDecoded\":0,\"explicitFrameOrTextureAllocations\":0,\"decoderInternalAllocationsObserved\":false,\"runningClientAdapterMatched\":false}\n",
      item.name,hardware?"true":"false",result.reason?"false":"true",result.reason?result.reason:"none",static_cast<long>(result.hresult),result.decoderStage,item.count,result.reason?"false":"true");
    if(result.reason&&(!std::strcmp(result.reason,"stream_memory_unknown")||!std::strcmp(result.reason,"stream_memory_budget")||!std::strcmp(result.reason,"stream_texture_budget")||!std::strcmp(result.reason,"stream_texture_unknown")))return 2;
    // Probe returns only after releasing the isolated MFT and selected devices;
    // the next case never holds the prior decoder or an allocated frame.
  }
  return 0; // Completed observations, not "every size supported" or a QA pass.
}
