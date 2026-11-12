#pragma once
#include "video_bounds.h"
#include <array>
namespace multitap {
struct StreamSize {uint32_t width=0,height=0,fps=0,sourceWidth=0,sourceHeight=0;};
struct StreamMemory {uint64_t systemBytes=0,gpuBytes=0;};
// Owners are bounded by the actual published/spare/composite/lossless buffers
// and 2 NV12 conversion/output buffers. GPU admission accounts for two in-flight
// decoded/source frames and the source overlay; pool growth checks actual budget
// again per allocation, rather than requiring all 10 pool slots up front.
// This is a resource check, not an allocation or a throughput promise.
inline bool StreamMemoryNeeded(const StreamSize* shapes,size_t count,StreamMemory& out){
  if(!shapes||!count||count>2)return false;StreamMemory total{};uint64_t totalRate=0;
  for(size_t i=0;i<count;++i){VideoLayout layout{};video::FrameLayout source{};const auto& s=shapes[i];
    if(!s.fps||!CheckedVideoLayout(s.width,s.height,layout))return false;
    uint64_t rate=0;if(!video::checkedMultiply(uint64_t(s.width)*s.height,s.fps,rate)||!video::checkedAdd(totalRate,rate,totalRate))return false;
    if(bool(s.sourceWidth)!=bool(s.sourceHeight)||!video::frameLayout(s.sourceWidth?s.sourceWidth:s.width,s.sourceHeight?s.sourceHeight:s.height,source,false))return false;
    if((s.sourceWidth&&s.sourceWidth<s.width)||(s.sourceHeight&&s.sourceHeight<s.height))return false;
    uint64_t cpu=0,gpu=0,term=0;
    if(!video::checkedMultiply(layout.rgbaBytes,3,cpu)||!video::checkedMultiply(source.rgbaBytes,5,term)||!video::checkedAdd(cpu,term,cpu)||
      !video::checkedAdd(cpu,uint64_t(layout.nv12Bytes)*2,cpu)||!video::checkedMultiply(layout.rgbaBytes,2,gpu)||
      !video::checkedMultiply(source.rgbaBytes,3,term)||!video::checkedAdd(gpu,term,gpu)||!video::checkedAdd(gpu,uint64_t(layout.nv12Bytes)*2,gpu))return false;
    if(!video::checkedAdd(total.systemBytes,cpu,total.systemBytes)||!video::checkedAdd(total.gpuBytes,gpu,total.gpuBytes))return false;
  }out=total;return true;
}
template<class ExactDecoder>
const char* AdmitStreamSizes(const StreamSize* shapes,size_t count,uint64_t availableSystem,ExactDecoder exact){
  StreamMemory bytes{};if(!StreamMemoryNeeded(shapes,count,bytes))return "stream_size_abi";
  if(!availableSystem)return "stream_memory_unknown";
  if(bytes.systemBytes>availableSystem)return "stream_memory_budget";
  for(size_t i=0;i<count;++i)if(const auto* reason=exact(shapes[i],i))return reason;
  return nullptr;
}
}
