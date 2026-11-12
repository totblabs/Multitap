#include "signin_backend_policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace host::signin;
int main(){unsigned checks=0;try{auto expect=[&](bool ok){++checks;if(!ok)throw std::runtime_error("compile availability bypassed a runtime admission guard");};
#if defined(MULTITAP_SIGNIN_BACKEND) && MULTITAP_SIGNIN_BACKEND
  expect(signinBackendCompiled);
#else
  expect(!signinBackendCompiled);
#endif
  expect(!signinBackendCanNegotiate(false));
  for(unsigned flags=0;flags<16;flags++){
    const bool enabled=bool(flags&1),broker=bool(flags&2),native=bool(flags&4),published=bool(flags&8);
    expect(signinBackendMayPublish(enabled,broker,native)==(signinBackendCompiled&&enabled&&broker&&native));
    expect(signinBackendMayAdmit(enabled,broker,native,published)==(signinBackendCompiled&&enabled&&broker&&native&&published));
  }
  expect(!signinBackendMayPublish(false,true,true));expect(!signinBackendMayPublish(true,false,true));expect(!signinBackendMayPublish(true,true,false));expect(!signinBackendMayAdmit(true,true,true,false));
  const auto status=signinBackendStatus(false);
  expect(status.find("\"backendCompiled\":"+std::string(signinBackendCompiled?"true":"false"))!=std::string::npos);
  expect(status.find("runtimeQualified")==std::string::npos);
  expect(status.find("\"defaultEnabled\":false")!=std::string::npos);
  expect(status.find("\"passwordRequired\":false")!=std::string::npos);
  expect(status.find("\"autologonRequired\":false")!=std::string::npos);
  std::cout<<"{\"kind\":\"signin-backend-compile-policy-pure\",\"backendCompiled\":"<<(signinBackendCompiled?"true":"false")<<",\"checks\":"<<checks<<",\"liveAcceptancePerformed\":false,\"nativeAdaptersInvoked\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
