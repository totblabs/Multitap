#pragma once
#include <string>
namespace host::signin {
// Build availability only. This flag is not a runtime qualification receipt.
#if defined(MULTITAP_SIGNIN_BACKEND) && MULTITAP_SIGNIN_BACKEND
inline constexpr bool signinBackendCompiled=true;
#else
inline constexpr bool signinBackendCompiled=false;
#endif
inline constexpr bool signinBackendCanNegotiate(bool enabled){return signinBackendCompiled&&enabled;}
inline constexpr bool signinBackendMayPublish(bool enabled,bool acceptedBroker,bool currentNative){return signinBackendCanNegotiate(enabled)&&acceptedBroker&&currentNative;}
inline constexpr bool signinBackendMayAdmit(bool enabled,bool acceptedBroker,bool currentNative,bool publishedScope){return signinBackendMayPublish(enabled,acceptedBroker,currentNative)&&publishedScope;}
inline std::string signinBackendStatus(bool enabled){
  return std::string("{\"protocol\":\"signin-settings-v1\",\"enabled\":")+(enabled?"true":"false")+
    ",\"defaultEnabled\":false,\"backendCompiled\":"+(signinBackendCompiled?"true":"false")+
    ",\"passwordRequired\":false,\"autologonRequired\":false,\"reason\":\""+(signinBackendCompiled?"signin_service_readiness_required":"signin_backend_not_compiled")+"\"}";
}
}
