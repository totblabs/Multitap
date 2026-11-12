#include "service.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
DisplayState readyDisplay(){DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";display.monitors={{"0",display.name,0,0,1920,1080,true}};return display;}
void cacheRoundtrip(){
  const auto baseline=readyDisplay();
  const std::vector<std::string> reasons{"viewport_recovery_required","viewport_settings_modified_externally","viewport_installed_driver_mismatch","driver_hash_mismatch","viewport_ccd_validate_failed","interactive_console_required"};
  for(const auto& reason:reasons){
    auto display=baseline;display.viewportProvisioning=true;
    DisplayHelperOutput failure;failure.exitCode=1;failure.value=JsonParser::parse("{\"error\":"+jsonQuote(reason)+",\"viewportProvisioningSupported\":false}");
    applyViewportProbe(display,failure,true,true,true);
    require(display.ready()&&!display.viewportProvisioning&&display.error==reason,"Viewport failure disabled fixed-resolution streaming or lost the actual fixed code");
    const auto parsed=parseDisplayState(JsonParser::parse(workerDisplayJson(display)));
    DisplayReadinessCache cache;const auto ticket=cache.begin(123);require(cache.complete(ticket,parsed,Clock::now(),true),"Current worker diagnostic did not enter the cache");
    const auto fromCache=cache.snapshot(123,true);
    require(fromCache.ready()&&!fromCache.viewportProvisioning&&viewportFailureReason(fromCache,true,true,true)==reason,"Worker JSON/cache discarded the viewport diagnostic");
    cache.invalidate();require(viewportFailureReason(cache.snapshot(123,true),true,true,true)!=reason,"Retired session retained an old worker's viewport error");
  }
}
void failureNormalization(){
  auto display=readyDisplay();DisplayHelperOutput failure;failure.exitCode=1;
  failure.value=JsonParser::parse(R"({"error":"PRIVATE_TOKEN=https://private.invalid/password"})");applyViewportProbe(display,failure,true,true,true);
  const auto wire=workerDisplayJson(display);require(display.error=="viewport_probe_failed"&&wire.find("PRIVATE_TOKEN")==std::string::npos&&wire.find("private.invalid")==std::string::npos,"Raw helper error escaped its fixed allowlist");
  failure.timedOut=true;applyViewportProbe(display,failure,true,true,true);require(display.ready()&&display.error=="viewport_probe_timeout","Helper timeout lost uncertainty or disabled ordinary streaming");
  failure.timedOut=false;failure.exitCode=0;failure.value=JsonParser::parse(R"({"viewportProvisioningSupported":true,"minWidth":320,"maxWidth":3840,"minHeight":180,"maxHeight":2160,"widthAlignment":2,"heightAlignment":2,"maxRefreshRate":60})");
  applyViewportProbe(display,failure,true,true,true);require(display.viewportProvisioning&&display.error.empty()&&viewportFailureReason(display,true,true,true).empty(),"Successful fresh helper probe retained a previous failure");
  applyViewportProbe(display,failure,true,true,false);require(display.ready()&&!display.viewportProvisioning&&display.error=="viewport_support_unavailable","An unprivileged worker advertised dynamic-resolution support");
}
void sessionAndProbeGates(){
  auto display=readyDisplay();require(viewportFailureReason(display,false,true,true)=="no_interactive_session","Locked/no-session gate was not distinguished");
  require(viewportFailureReason(display,true,true,false)=="interactive_console_required","RDP/console gate was not distinguished");
  require(viewportFailureReason(display,true,false,true)=="viewport_probe_pending","An uncompleted worker probe invented a helper failure");
  display.error="viewport_ccd_validate_failed";require(viewportFailureReason(display,true,false,true)==display.error,"Capture unavailability hid an actual helper failure");
}
void restartAndNativeCauses(){
  auto display=readyDisplay();display.rebootRequired=true;display.error="interactive_console_required";
  for(const bool interactive:{false,true})for(const bool capture:{false,true})for(const bool console:{false,true})
    require(viewportFailureReason(display,interactive,capture,console)=="reboot_required","Session/capture gate hid an actual devnode restart requirement");
  display.rebootRequired=false;
  require(viewportFailureReason(display,true,true,false)=="interactive_console_required","Cleared restart retained an obsolete reboot reason");
  display.error.clear();display.active=false;
  require(viewportFailureReason(display,true,false,true)=="owned_virtual_display_not_ready","Missing owned output was mistaken for a pending capture probe");
  display.recoveryRequired=true;
  require(viewportFailureReason(display,true,false,true)=="viewport_recovery_required","Required recovery was hidden by output/capture state");
  const std::vector<std::string> reasons{"native_display_not_ready","native_display_provider_unavailable","native_display_snapshot_invalid","native_display_configuration_untrusted","native_display_topology_mismatch","native_display_change_pending","native_topology_not_ready","native_topology_recovery_required"};
  for(const auto& reason:reasons){
    display.error=reason;
    const auto decoded=parseDisplayState(JsonParser::parse(workerDisplayJson(display)));
    require(!decoded.ready()&&viewportFailureReason(decoded,true,false,true)==reason,"Native diagnosis was dropped or granted output readiness");
  }
}
}
int main(){try{cacheRoundtrip();failureNormalization();sessionAndProbeGates();restartAndNativeCauses();std::cout<<"{\"kind\":\"viewport-diagnostic-preservation\",\"failureCacheRoundtrips\":6,\"nativeCausesPreserved\":8,\"devnodeRestartOutranksSessionGate\":true,\"fixedResolutionStreamingPreserved\":true,\"unknownRawHelperErrorsSuppressed\":true,\"timeoutAndRecoveryDistinct\":true,\"successClearsOldFailure\":true,\"sessionGenerationInvalidatesOldDiagnostic\":true,\"actualHelperInvoked\":false,\"actual149CauseDetermined\":false,\"serviceDeviceRegistryChanges\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
