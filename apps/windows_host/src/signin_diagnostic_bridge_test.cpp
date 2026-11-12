// All admission/capture callbacks are in-memory. A regressed gate still reaches
// only the existing test admission model or the synthetic ThumbnailShooter.
#include "service.hpp"
#include <iostream>

namespace host {
struct SessionStopTestAccess {
  static void model(ServiceBridge& bridge){bridge.qemuAdmissionModel_=true;}
  static unsigned admissions(const ServiceBridge& bridge){return bridge.qemuStreamAdmissions_;}
  static uint32_t thumbnails(const ServiceBridge& bridge){return bridge.thumbnailActive_.load();}
  static unsigned probes(const ServiceBridge& bridge){return bridge.qemuProbeAdmissions_;}
  static bool probing(const ServiceBridge& bridge){return bridge.probing_.load();}
  static void beforeProbe(ServiceBridge& bridge,std::function<void()> action){bridge.diagnosticProbeInterleave_=std::move(action);}
  static void probe(ServiceBridge& bridge){bridge.probeDisplay(17,true);}
  static void thumbnail(ServiceBridge& bridge,const ThumbnailShooter::Observe& observe,
      const ThumbnailShooter::Capture& capture){
    bridge.thumbnailShooter_.~ThumbnailShooter();
    std::construct_at(&bridge.thumbnailShooter_,observe,capture,[](const ThumbnailShot&){});
  }
};
}
namespace {
unsigned checks=0;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
int cases(){
  using namespace host;
  auto activity=std::make_shared<std::atomic<bool>>(true);
  ServiceBridge bridge(Credentials{});bridge.diagnosticActivity(activity);SessionStopTestAccess::model(bridge);
  unsigned observed=0,captured=0;bool revokeDuringCapture=false;
  SessionStopTestAccess::thumbnail(bridge,[&]{++observed;return ThumbnailSession{1,true,true};},[&](DWORD){
    ++captured;require(SessionStopTestAccess::thumbnails(bridge)==1,"In-flight thumbnail was invisible to final admission");
    if(revokeDuringCapture)activity->store(true);
    return JsonParser::parse("{\"state\":\"blank\"}");
  });
  require(bridge.privateDiagnosticBusy()&&bridge.hostUpdateSessionActive(),"Diagnostic activity did not defer Host replacement");
  require(bridge.thumbnail().state==ThumbnailState::busy&&observed==0&&captured==0&&SessionStopTestAccess::thumbnails(bridge)==0,
    "Busy diagnostic admitted preview acquisition or leaked its activity counter");
  std::vector<Json> packets;
  const auto start=JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"00000000-0000-0000-0000-000000000001\"}");
  const auto send=[&](bool text,std::vector<uint8_t> bytes){if(text)packets.push_back(JsonParser::parse(std::string(bytes.begin(),bytes.end())));};
  bridge.start(start,send);
  require(SessionStopTestAccess::admissions(bridge)==0&&packets.size()==1&&
    packets.front().get("reason").string()=="host_maintenance","Busy diagnostic reached ordinary stream admission");
#ifdef MULTITAP_NATIVE_DISPLAY
  require(bridge.nativeResetBusy()=="recovery_running","Existing reset could overlap a diagnostic owner");
#endif
  activity->store(false);packets.clear();bridge.start(start,send);
  require(!bridge.privateDiagnosticBusy()&&!bridge.hostUpdateSessionActive()&&
    SessionStopTestAccess::admissions(bridge)==1&&packets.empty(),"Released diagnostic changed ordinary stream admission");
  revokeDuringCapture=true;
  const auto shot=bridge.thumbnail();
  require(captured==1&&observed==2&&shot.state!=ThumbnailState::captured&&shot.state!=ThumbnailState::blank&&
    SessionStopTestAccess::thumbnails(bridge)==0,"Preview was published after diagnostic activity won the race");
  require(bridge.privateDiagnosticBusy()&&bridge.hostUpdateSessionActive(),"Revoked preview lost the persistent diagnostic owner");
  ServiceBridge replacement(Credentials{});replacement.diagnosticActivity(activity);SessionStopTestAccess::model(replacement);
  replacement.start(start,send);
  require(replacement.privateDiagnosticBusy()&&SessionStopTestAccess::admissions(replacement)==0&&packets.size()==1,
    "Recreated bridge bypassed the service's retained diagnostic owner");
  activity->store(false);require(!replacement.privateDiagnosticBusy(),"Recreated bridge did not observe completion");
  bool reachedProbeGap=false;
  SessionStopTestAccess::beforeProbe(replacement,[&]{
    reachedProbeGap=true;activity->store(true);
    require(!SessionStopTestAccess::probing(replacement),"Interleave did not reach the original admission gap");
  });
  SessionStopTestAccess::probe(replacement);
  require(reachedProbeGap&&SessionStopTestAccess::probes(replacement)==0&&
    !SessionStopTestAccess::probing(replacement),"Probe crossed a diagnostic that won after the initial busy check");
  activity->store(false);SessionStopTestAccess::beforeProbe(replacement,{});
  SessionStopTestAccess::probe(replacement);
  require(SessionStopTestAccess::probes(replacement)==1&&!SessionStopTestAccess::probing(replacement),
    "Ordinary probe admission failed after the diagnostic released");
  return 0;
}
}
int main(){
  try{cases();std::cout<<"{\"kind\":\"signin-diagnostic-bridge-admission\",\"checks\":"<<checks<<",\"captureCallbacks\":\"synthetic\",\"actualWorkers\":0,\"actualDesktopCapture\":false,\"driverCalls\":0,\"inputEvents\":0,\"runtimeQualified\":false}\n";return 0;}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
