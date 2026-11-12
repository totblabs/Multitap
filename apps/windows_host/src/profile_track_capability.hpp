#pragma once
#include "initial_profile_tracks.hpp"

namespace host {
// Advisory advertisement only. Session preparation and each resize still
// verify a fresh protected lease, profile journal and actual output bindings.
inline bool nativeStreamProfileTracksCapability(const DisplayState& display,const Json& config,bool nativeSelected,
    bool eligible,bool activeProfile,const Json& snapshot,DWORD servicePid){
  try{
    if(!initialProfileTracksEnabled(config,nativeSelected)||!nativeConfigMatchesLease(config,snapshot)||!eligible||!display.ready()||display.remoteSession||
       !display.viewportProvisioning||(!activeProfile&&!display.monitorControlSupported)||
       display.monitors.empty()||display.monitors.size()>2)return false;
    NativeTrackResize actual;multitap::tracks::Configuration shape;shape.topology=1;
    for(const auto& m:display.monitors){
      if((m.id!="0"&&m.id!="1")||!ownedOutputSize(m.width,m.height))return false;
      actual.outputs.push_back({m.id,uint32_t(m.width),uint32_t(m.height),m.left,m.top});
      shape.tracks.push_back({uint8_t(m.id=="0"?0:1),1,uint32_t(m.width),uint32_t(m.height),
        uint32_t(m.width),uint32_t(m.height),m.left,m.top,m.primary});
    }
    if(!multitap::tracks::valid(shape))return false;
    std::sort(actual.outputs.begin(),actual.outputs.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    return initialProfileTrackDisplayMatches(display,actual,activeProfile)&&
      initialProfileTrackLeaseMatches(snapshot,display,actual,servicePid,snapshot.get("deviceInstanceId").string());
  }catch(...){return false;}
}
// The owned group is "active" ("0" primary at the origin, count changes off)
// while a v1 profile reservation or a display manager v2 stream owns the
// displays. v2 holds no reservation or journal; its stream flag is enough.
inline bool nativeStreamProfileTracksActive(bool profileSession,bool v2Stream,bool stopped,bool streaming){return (profileSession||v2Stream)&&!stopped&&streaming;}
inline std::string nativeStreamProfileTracksCapabilityFields(bool enabled,bool profileAlreadyDeclared){
  if(!enabled)return "";
  return std::string(profileAlreadyDeclared?"":",\"streamProfileProtocol\":\"console-profile-v1\"")+
    ",\"streamProfileTracksProtocol\":\"group-v1\",\"videoTrackProtocol\":\"tracks-v1\",\"trackViewportProvisioning\":true";
}
}
