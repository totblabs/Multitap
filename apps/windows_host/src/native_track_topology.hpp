#pragma once
#include "native_display_topology.hpp"
#include "owned_output_size.hpp"

namespace host {
struct NativeTrackViewport {std::string id;uint32_t width=0,height=0;LONG left=0,top=0;};
struct NativeTrackViewportPlan {
  std::vector<NativeDisplayTarget> targets;
  std::vector<NativeOutputPlacement> placements;
};
// Plan all currently owned outputs as one relative group. A shared translation
// may keep the console at its original coordinates, but individual tracks must
// never be rearranged or stretched. Count changes are a separate transaction.
inline NativeTrackViewportPlan planNativeTrackViewport(const NativeTopologyBaseline& baseline,std::span<const NativeTrackViewport> requested){
  const auto fail=[] {throw std::runtime_error("native_track_viewport_invalid");};
  if(requested.empty()||requested.size()>2||requested.size()!=baseline.targets.size())fail();
  const auto captured=captureNativeTopologyBaseline(baseline.ccd,baseline.targets,baseline.replacedMonitor);
  if(captured.placements.size()!=baseline.placements.size())fail();
  for(size_t i=0;i<captured.placements.size();++i){const auto& a=captured.placements[i];const auto& b=baseline.placements[i];if(a.id!=b.id||a.left!=b.left||a.top!=b.top||a.primary!=b.primary)fail();}
  NativeTrackViewportPlan result;result.targets=baseline.targets;
  std::vector<NativeTrackViewport> ordered;
  for(size_t i=0;i<baseline.targets.size();++i){
    const auto id=std::to_string(i);const auto found=std::find_if(requested.begin(),requested.end(),[&](const auto& value){return value.id==id;});
    if(found==requested.end()||std::count_if(requested.begin(),requested.end(),[&](const auto& value){return value.id==id;})!=1||!ownedOutputSize(found->width,found->height)||!displayRectangleFits(found->left,found->top,found->width,found->height))fail();
    ordered.push_back(*found);result.targets[i].width=found->width;result.targets[i].height=found->height;
  }
  const auto intersects=[](int64_t ax,int64_t ay,int64_t aw,int64_t ah,int64_t bx,int64_t by,int64_t bw,int64_t bh){return ax<bx+bw&&bx<ax+aw&&ay<by+bh&&by<ay+ah;};
  if(ordered.size()==2&&intersects(ordered[0].left,ordered[0].top,ordered[0].width,ordered[0].height,ordered[1].left,ordered[1].top,ordered[1].width,ordered[1].height))fail();
  const auto foreign=nativeForeignCcd(baseline.ccd,baseline.targets);
  const auto primary=std::find_if(baseline.placements.begin(),baseline.placements.end(),[](const auto& value){return value.primary;});
  const auto anchor=primary==baseline.placements.end()?baseline.placements.begin():primary;
  const auto selected=std::find_if(ordered.begin(),ordered.end(),[&](const auto& value){return value.id==anchor->id;});
  if(selected==ordered.end())fail();
  const auto accept=[&](int64_t dx,int64_t dy){
    std::vector<NativeOutputPlacement> placements;
    for(const auto& item:ordered){
      const int64_t x=int64_t(item.left)+dx,y=int64_t(item.top)+dy;
      if(!displayRectangleFits(x,y,item.width,item.height))return false;
      for(const auto& path:foreign.paths){const auto& source=nativeCcdSource(foreign,path);if(intersects(x,y,item.width,item.height,source.position.x,source.position.y,source.width,source.height))return false;}
      placements.push_back({item.id,LONG(x),LONG(y),!x&&!y});
    }
    if(primary!=baseline.placements.end()){
      const auto root=std::find_if(placements.begin(),placements.end(),[&](const auto& value){return value.id==primary->id;});if(root==placements.end()||!root->primary)return false;
    }
    result.placements=std::move(placements);return true;
  };
  const int64_t originalDx=int64_t(anchor->left)-selected->left,originalDy=int64_t(anchor->top)-selected->top;
  if(accept(originalDx,originalDy))return result;
  // An owned primary cannot be relocated without changing the desktop origin.
  // With a preserved foreign primary, try only translations of the whole group.
  if(primary!=baseline.placements.end()||foreign.paths.empty())fail();
  int64_t left=INT64_MAX,top=INT64_MAX,right=INT64_MIN,bottom=INT64_MIN,groupLeft=INT64_MAX,groupTop=INT64_MAX,groupRight=INT64_MIN,groupBottom=INT64_MIN;
  for(const auto& path:foreign.paths){const auto& source=nativeCcdSource(foreign,path);left=std::min(left,int64_t(source.position.x));top=std::min(top,int64_t(source.position.y));right=std::max(right,int64_t(source.position.x)+source.width);bottom=std::max(bottom,int64_t(source.position.y)+source.height);}
  for(const auto& item:ordered){groupLeft=std::min(groupLeft,int64_t(item.left));groupTop=std::min(groupTop,int64_t(item.top));groupRight=std::max(groupRight,int64_t(item.left)+item.width);groupBottom=std::max(groupBottom,int64_t(item.top)+item.height);}
  for(const auto& offset:{std::pair{right-groupLeft,originalDy},std::pair{left-groupRight,originalDy},std::pair{originalDx,bottom-groupTop},std::pair{originalDx,top-groupBottom}})if(accept(offset.first,offset.second))return result;
  fail();return {};
}
}
