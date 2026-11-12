#pragma once
#include "native_display_topology.hpp"
#include "native_stream_console_identity.hpp"

namespace host {
// This is a geometry planner, not authority. The caller must hold the exact
// reserved stream profile and revalidate its protected journal, service/worker
// lifetime, machine, login and console device before EVERY Windows operation.
// Only that profile's positively identified console may be suppressed again
// when replacing the sole active owned SDK target makes it reappear.
inline NativeTopologyPlan planNativeStreamViewportConsole(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets,const NativeConsoleBinding& console,std::span<const NativeOutputPlacement> exact={}){
  if(!canonicalNativeConsoleBinding(console))throw std::runtime_error("native_stream_console_profile_changed");
  auto filtered=fresh;size_t active=0;
  filtered.paths.erase(std::remove_if(filtered.paths.begin(),filtered.paths.end(),[&](const auto& path){
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||!nativeSameLuid(path.targetInfo.adapterId,console.path.targetAdapter)||path.targetInfo.id!=console.path.target)return false;
    if(!nativeSameLuid(path.sourceInfo.adapterId,console.path.sourceAdapter)||path.sourceInfo.id!=console.path.source||!path.targetInfo.targetAvailable||nativeOwnedPath(path,targets)||++active!=1)throw std::runtime_error("native_stream_console_profile_changed");
    (void)nativeCcdSource(fresh,path);(void)nativeCcdTarget(fresh,path);return true;
  }),filtered.paths.end());
  return planNativeTopologyTransition(baseline,filtered,targets,exact);
}
}
