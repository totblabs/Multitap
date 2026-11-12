#pragma once
#include "native_display_topology.hpp"
#include <array>
#include <span>
#include <utility>

namespace host {
// Fixed, non-identifying copies only. Capturing evidence never queries Windows,
// writes the journal, allocates strings, or changes a display operation's result.
struct DisplayV2GeometryEvidence {
  uint32_t track=0,width=0,height=0,left=0,top=0;
  bool observed=false,active=false,primary=false;
  bool operator==(const DisplayV2GeometryEvidence&)const=default;
};
struct DisplayV2GeometrySet {
  std::array<DisplayV2GeometryEvidence,2> rows{};
  uint32_t count=0;bool captured=false;
  bool operator==(const DisplayV2GeometrySet&)const=default;
};
struct DisplayV2SnapshotEvidence {
  DisplayV2GeometrySet before,saved;
  bool reusedExisting=false,olderSession=false;
  bool operator==(const DisplayV2SnapshotEvidence&)const=default;
};
inline DisplayV2GeometrySet displayV2GeometryEvidence(const NativeCcdData& ccd,std::span<const NativeDisplayTarget> owned)noexcept{
  DisplayV2GeometrySet out;
  if(owned.empty()||owned.size()>2)return out;
  bool seen[2]{};
  for(const auto& target:owned){
    if(target.id!="0"&&target.id!="1")return {};
    const uint32_t track=target.id=="0"?0u:1u;
    if(seen[track])return {};seen[track]=true;
    auto& row=out.rows[out.count++];row.track=track;
    try{
      for(const auto& path:ccd.paths){
        if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||!nativeSameTarget(path,target))continue;
        row.active=true;
        const auto& source=nativeCcdSource(ccd,path);
        if(!source.width||!source.height||source.width>32768||source.height>32768)break;
        row.width=source.width;row.height=source.height;
        // Conversion to uint32 preserves the signed int32 coordinate's bits.
        row.left=uint32_t(source.position.x);row.top=uint32_t(source.position.y);
        row.primary=source.position.x==0&&source.position.y==0;row.observed=true;break;
      }
    }catch(...){ }
  }
  if(out.count==2&&out.rows[0].track>out.rows[1].track)std::swap(out.rows[0],out.rows[1]);
  out.captured=true;return out;
}
}
