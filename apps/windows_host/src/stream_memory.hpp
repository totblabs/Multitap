#pragma once
#include "owned_output_size.hpp"
#include <algorithm>
#include <optional>
#include <span>

namespace host {
struct StreamMemoryShape {
  uint32_t sourceWidth=0,sourceHeight=0,width=0,height=0;
  // Single WGC can expose source-sized lossless pixels even while H264 is
  // scaled. Tracks currently permit tiles only when picture == source.
  bool losslessSource=true;
};
// Conservative admission forecast, not a claim about exact opaque allocations.
// Sum the existing source4BGRA, capture4BGRA+3NV12, max(MFT4,x26412)NV12
// forecasts, plus explicit policy1NV12 and eligible lossless source1BGRA.
// Some workspaces can overlap; this intentionally does not subtract them.
// Actual codec/driver allocation overhead remains unknown, and availability can
// change after admission. Existing per-allocation checks must remain in place.
inline constexpr std::optional<uint64_t> streamMemoryForecast(std::span<const StreamMemoryShape> shapes){
  if(shapes.empty()||shapes.size()>2)return {};
  uint64_t total=0;
  const auto add=[&](uint64_t bytes,uint64_t copies){uint64_t group=0;return multitap::video::checkedMultiply(bytes,copies,group)&&multitap::video::checkedAdd(total,group,total);};
  for(const auto& shape:shapes){
    const auto source=checkedFrameLayout(shape.sourceWidth,shape.sourceHeight,false),encoded=checkedFrameLayout(shape.width,shape.height);
    if(shape.sourceWidth<16||shape.sourceHeight<16||shape.width<16||shape.height<16||shape.width>shape.sourceWidth||shape.height>shape.sourceHeight||!source||!encoded)return {};
    if(!add(source->bgraBytes,4)||!add(encoded->bgraBytes,4)||!add(encoded->nv12Bytes,3)||
       !add(encoded->nv12Bytes,std::max<uint64_t>(4,12))||!add(encoded->nv12Bytes,1)||
       (shape.losslessSource&&!add(source->bgraBytes,1)))return {};
  }
  return total;
}
enum class StreamMemoryAdmission {accepted,invalidLayout,unavailable,insufficient};
// Adaptive single-picture changes keep one immutable source and lossless
// eligibility. Only a larger checked forecast needs new aggregate admission.
// Invalid/changed-source proposals never receive a teardown exemption.
inline constexpr std::optional<bool> streamMemoryGrowthRequiresAdmission(const StreamMemoryShape& current,const StreamMemoryShape& candidate){
  if(current.sourceWidth!=candidate.sourceWidth||current.sourceHeight!=candidate.sourceHeight||current.losslessSource!=candidate.losslessSource)return {};
  const auto before=streamMemoryForecast({&current,1}),after=streamMemoryForecast({&candidate,1});
  if(!before||!after)return {};
  return *after>*before;
}
inline constexpr StreamMemoryAdmission streamMemoryAdmission(std::optional<uint64_t> forecast,std::optional<uint64_t> available){
  if(!forecast)return StreamMemoryAdmission::invalidLayout;
  if(!available)return StreamMemoryAdmission::unavailable;
  return *forecast<=*available?StreamMemoryAdmission::accepted:StreamMemoryAdmission::insufficient;
}
inline constexpr const char* streamMemoryFailure(StreamMemoryAdmission result){
  switch(result){
    case StreamMemoryAdmission::accepted:return nullptr;
    case StreamMemoryAdmission::invalidLayout:return "frame_layout_unsupported";
    case StreamMemoryAdmission::unavailable:return "frame_memory_unavailable";
    case StreamMemoryAdmission::insufficient:return "frame_memory_insufficient";
  }
  return "frame_layout_unsupported";
}
}
