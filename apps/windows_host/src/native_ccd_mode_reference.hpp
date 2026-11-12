#pragma once
#include <windows.h>
#include <span>
namespace host {
inline size_t normalizeNativeCcdModeReferences(std::span<DISPLAYCONFIG_PATH_INFO> paths,std::span<const DISPLAYCONFIG_MODE_INFO> modes){
  size_t changed=0;
  for(auto& path:paths){
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE))continue;
    const UINT32 sourceRaw=path.sourceInfo.modeInfoIdx,targetRaw=path.targetInfo.modeInfoIdx;
    if(sourceRaw<modes.size()||targetRaw<modes.size()||
       (sourceRaw&0xffff)!=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID||(targetRaw&0xffff)!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)continue;
    const UINT32 source=sourceRaw>>16,target=targetRaw>>16;
    if(source>=modes.size()||target>=modes.size())continue;
    const auto& s=modes[source];const auto& t=modes[target];
    if(s.infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE||t.infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET||
       s.adapterId.LowPart!=path.sourceInfo.adapterId.LowPart||s.adapterId.HighPart!=path.sourceInfo.adapterId.HighPart||s.id!=path.sourceInfo.id||
       t.adapterId.LowPart!=path.targetInfo.adapterId.LowPart||t.adapterId.HighPart!=path.targetInfo.adapterId.HighPart||t.id!=path.targetInfo.id)continue;
    // Observed vGPU queries pack both indices without the per-path flag.
    // Exact matching modes prove the references; keep legacy flags for SDC.
    path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;++changed;
  }
  return changed;
}
}
