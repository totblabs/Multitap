#pragma once
#include "initial_viewport.hpp"
#include "owned_output_size.hpp"

namespace host {
inline bool nativeStreamProfileViewportCapability(const DisplayState& display,const Json& config,bool nativeSelected,bool eligible,bool activeProfile){
  if(!nativeSelected||!eligible||!display.ready()||display.remoteSession||!display.viewportProvisioning||(!activeProfile&&!display.monitorControlSupported)||display.monitors.size()!=1||display.monitors.front().id!="0"||!std::holds_alternative<bool>(config.get("streamProfileTrialEnabled").value)||!config.get("streamProfileTrialEnabled").boolean()||config.get("provider").string()!="libvirtualdisplay-1.6.4"||!std::holds_alternative<bool>(config.get("enabled").value)||!config.get("enabled").boolean())return false;
  const auto& output=display.monitors.front();return ownedOutputSize(output.width,output.height)&&(!activeProfile||output.primary);
}
// stream-size-v1 extends separate-v1: full-size encoding within the client's
// decoder limits and measured load, and portrait owned outputs (stream_size.hpp).
// The service base already declares streamSizeProtocol; duplicating it makes
// the strict outgoing parser reject display-result capabilities after 2 -> 1.
inline std::string nativeStreamProfileCapabilityFields(bool enabled){return enabled?",\"streamProfileProtocol\":\"console-profile-v1\",\"streamProfileViewportProtocol\":\"epoch-v1\",\"initialViewportProtocol\":\"epoch-v1\",\"viewportEncoderLimitsProtocol\":\"separate-v1\"":"";}
}
