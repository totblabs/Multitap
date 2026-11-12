#include "runtime.hpp"
#include "windows_endpoint.hpp"
#include <virtual_display/driver/lease_store.h>
#include <virtual_display/driver/edid.h>
#include <virtual_display/driver/windows_driver_modes.h>
#include <iostream>
#include <source_location>
using namespace multitap::vdisplay;
constexpr uint64_t ownedLease=api::kMinOpaqueLeaseId+123,foreignLease=api::kMinOpaqueLeaseId+987;
void check(bool condition,const std::source_location source=std::source_location::current()){if(!condition)throw std::runtime_error("native display controller regression at line "+std::to_string(source.line()));}
// Actual upstream lease/ownership validation under the wire serializer, with
// simulated OS departures. No kernel/IddCx or certificate calls are made.
struct Transport:api::ControlTransport {
  api::DisplayStore store{2,8};
  int creates=0,releases=0,removes=0,feeds=0,queries=0,failCreate=0,delay=2;
  bool failFeed=false,failRelease=false,failQuery=false,stuckDeparture=false,badReply=false;
  struct Departure{api::LeaseDisplayRequest request;int rounds;};
  std::vector<Departure> pending;
  std::map<uint64_t,uint32_t> targetIds;
  bool ok(api::StoreResult result,uint32_t& error){error=result.error==api::StoreError::None?0:5;return !error;}
  void foreign(){api::CreateTemporaryDisplayOwnedRequest r{};r.display.lease_id=foreignLease;r.display.display_id=999;r.display.width=1280;r.display.height=720;r.display.physical_width_mm=600;r.display.physical_height_mm=340;r.display.refresh_rate_millihz=60000;r.display.requested_timeout_ms=15000;std::memcpy(r.display.display_name,"Foreign display",16);r.owner_capability.bytes.fill(9);check(store.create_temporary_display_owned(r,std::chrono::steady_clock::now()).status.error==api::StoreError::None);}
  void preserved(){const auto f=store.find_temporary_display(999);check(f&&f->lease_id==foreignLease&&f->width==1280&&f->height==720&&!f->pending_departure);}
  bool ioctl(uint32_t code,const void* input,size_t,void* output,size_t,size_t& size,uint32_t& error)override{
    size=0;error=0;const auto now=std::chrono::steady_clock::now();
    if(code==api::kIoctlGetProtocolVersion){*static_cast<api::ProtocolVersion*>(output)={};size=sizeof(api::ProtocolVersion);return true;}
    if(code==api::kIoctlQueryLease){++queries;if(failQuery){error=5;return false;}
      if(!stuckDeparture)for(auto it=pending.begin();it!=pending.end();){if(--it->rounds<=0){check(ok(store.remove_temporary_display(it->request),error));it=pending.erase(it);}else ++it;}
      *static_cast<api::QueryLeaseResult*>(output)=store.query_lease(static_cast<const api::LeaseRequest*>(input)->lease_id,now);size=sizeof(api::QueryLeaseResult);return true;}
    if(code==api::kIoctlCreateTemporaryDisplayOwned){++creates;if(creates==failCreate){error=5;return false;}
      const auto& request=*static_cast<const api::CreateTemporaryDisplayOwnedRequest*>(input);auto result=store.create_temporary_display_owned(request,now);
      if(!ok(result.status,error))return false;result.result.target_id=uint32_t(creates);result.result.os_adapter_luid.low_part=44;
      targetIds[request.display.display_id]=result.result.target_id;
      if(badReply)result.result.lease_id++;
      *static_cast<api::CreateTemporaryDisplayResult*>(output)=result.result;size=sizeof(result.result);return true;}
    if(code==api::kIoctlRemoveTemporaryDisplay){++removes;const auto request=*static_cast<const api::LeaseDisplayRequest*>(input);check(request.lease_id==ownedLease&&request.display_id!=999);
      if(!ok(store.mark_temporary_display_pending_departure(request),error))return false;pending.push_back({request,delay});return true;}
    if(code==api::kIoctlFeedLease){++feeds;if(failFeed){error=5;return false;}return ok(store.feed_lease(*static_cast<const api::LeaseRequest*>(input),now),error);}
    if(code==api::kIoctlReleaseLease){++releases;const auto request=*static_cast<const api::LeaseRequest*>(input);check(request.lease_id==ownedLease);if(failRelease){error=5;return false;}return ok(store.release_lease(request),error);}
    throw std::runtime_error("unexpected global/foreign driver operation");
  }
};
int main(){try{
  api::OwnerCapability owner{};owner.bytes[0]=42;const Output original{71,1920,1080};int passed=0;
  {Transport t;t.foreign();api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));const auto first=controller.state().targets[0].target_id;
   const Output dual[]{original,{72,1280,720}};check(controller.reconfigure(dual)&&controller.state().targets[0].target_id==first&&t.removes==0&&t.releases==0);
   check(controller.reconfigure({&original,1})&&controller.state().targets[0].target_id==first&&t.removes==1&&t.releases==0);t.preserved();++passed;}
  {Transport t;t.foreign();api::ControlClient client(t);Controller controller(client,ownedLease,owner);const Output dual[]{original,{72,1280,720}};check(controller.start(dual));const auto neighbor=controller.state().targets[1].target_id;
   const Output resized[]{{71,1600,900},dual[1]};check(controller.reconfigure(resized));const auto state=controller.state();check(state.targets[0].display_id==71&&state.targets[1].target_id==neighbor&&t.removes==1&&t.releases==0);check(t.store.find_temporary_display(71)->width==1600);t.preserved();++passed;}
  {Transport t;api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));const int creates=t.creates,queries=t.queries;check(controller.reconfigure({&original,1})&&t.creates==creates&&t.queries==queries&&t.removes==0&&t.releases==0);++passed;}
  {Transport t;api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));const Output invalid{71,1599,900};check(!controller.reconfigure({&invalid,1})&&t.removes==0&&t.releases==0&&controller.state().leaseActive);++passed;}
  {Transport t;t.foreign();api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));t.failCreate=2;const Output desired{71,1600,900};check(!controller.reconfigure({&desired,1}));check(controller.state().leaseActive&&!controller.state().recoveryRequired&&t.store.find_temporary_display(71)->width==1920&&t.releases==0);t.preserved();++passed;}
  {Transport t;t.delay=4;api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));const Output desired{71,1600,900};check(controller.reconfigure({&desired,1})&&t.feeds>=3&&t.queries>=5&&t.creates==2&&t.releases==0);++passed;}
  {Transport t;t.foreign();api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));t.failFeed=true;check(!controller.heartbeat()&&!controller.state().leaseActive&&controller.state().targets.empty()&&controller.state().recoveryRequired);t.preserved();++passed;}
  {Transport t;api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));t.failRelease=true;check(!controller.stop()&&controller.state().targets.empty()&&controller.state().recoveryRequired);t.failRelease=false;check(controller.stop());++passed;}
  {Transport t;api::CreateTemporaryDisplayOwnedRequest foreign{};foreign.display.lease_id=ownedLease;foreign.display.display_id=888;foreign.display.width=1280;foreign.display.height=720;foreign.display.physical_width_mm=600;foreign.display.physical_height_mm=340;foreign.display.refresh_rate_millihz=60000;std::memcpy(foreign.display.display_name,"Foreign display",16);foreign.owner_capability.bytes.fill(9);check(t.store.create_temporary_display_owned(foreign,std::chrono::steady_clock::now()).status.error==api::StoreError::None);
   api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(!controller.start({&original,1})&&t.creates==0&&t.releases==0&&t.store.find_temporary_display(888).has_value());++passed;}
  {Transport t;api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));const auto first=controller.state().targets[0].target_id;t.failCreate=2;const Output dual[]{original,{72,1280,720}};check(!controller.reconfigure(dual)&&controller.state().leaseActive&&controller.state().targets[0].target_id==first&&t.removes==0&&t.releases==0);++passed;}
  {Transport t;api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));t.badReply=true;const Output dual[]{original,{72,1280,720}};check(!controller.reconfigure(dual)&&!controller.state().leaseActive&&controller.state().recoveryRequired&&controller.state().targets.empty());++passed;}
  {Transport t;t.foreign();api::ControlClient client(t);Controller controller(client,ownedLease,owner,std::chrono::milliseconds(0));check(controller.start({&original,1}));t.stuckDeparture=true;const Output desired{71,1600,900};check(!controller.reconfigure({&desired,1})&&!controller.state().leaseActive&&controller.state().recoveryRequired&&controller.state().targets.empty()&&t.creates==1&&t.releases==0);t.preserved();++passed;}
  // stream-size-v1: portrait owned outputs (a phone's 1080x2400, rotated 4K)
  // pass actual upstream store validation with a portrait panel size. The
  // provider's protocol envelope remains separate from the product pixel policy.
  {Transport t;t.foreign();api::ControlClient client(t);Controller controller(client,ownedLease,owner);check(controller.start({&original,1}));
   const Output phone{71,1080,2400};check(controller.reconfigure({&phone,1}));const auto shown=t.store.find_temporary_display(71);check(shown&&shown->width==1080&&shown->height==2400&&shown->physical_width_mm==340&&shown->physical_height_mm==600);
   const Output rotated{71,2160,3840};check(controller.reconfigure({&rotated,1})&&t.store.find_temporary_display(71)->height==3840);
   const Output square{71,3840,3840};check(controller.reconfigure({&square,1})&&controller.state().leaseActive);
   const int creates=t.creates;const Output unsupported{71,4320,7680};check(!controller.reconfigure({&unsupported,1})&&t.creates==creates&&controller.state().leaseActive);
   const Output landscape{71,1920,1080};check(controller.reconfigure({&landscape,1})&&t.store.find_temporary_display(71)->physical_width_mm==600);t.preserved();++passed;}
  // The pinned driver source builds a portrait output's EDID detailed timing
  // and its preferred target mode from the requested width/height (identity
  // rotation): 1080x2400 and 2160x3840 stay valid 60 Hz modes (2160x3840 has
  // the same 627 MHz pixel clock as 3840x2160, below the 655.35 MHz DTD cap).
  for(const auto& [w,h]:std::array<std::pair<uint32_t,uint32_t>,3>{{{1080,2400},{2160,3840},{3840,2160}}}){
    api::EdidOptions options;options.width=w;options.height=h;options.physical_width_mm=h>w?340:600;options.physical_height_mm=h>w?600:340;options.refresh_rate_millihz=60000;
    const auto edid=api::create_edid(options);check(api::has_valid_edid_checksums(edid));
    const auto timing=api::read_preferred_timing(edid);check(timing.horizontal_active==w&&timing.vertical_active==h);
    const double hz=timing.pixel_clock_10khz*10000.0/(double(timing.horizontal_active+timing.horizontal_blanking)*double(timing.vertical_active+timing.vertical_blanking));check(hz>59.5&&hz<60.5);
    const auto requested=api::active_windows_driver_mode_shape(w,h,60000);const auto [modes,preferred]=api::build_windows_driver_target_mode_shapes(std::nullopt,&requested);
    check(preferred<modes.size()&&modes[preferred].width==w&&modes[preferred].height==h&&modes[preferred].refresh_rate_millihz==60000);
  }
  ++passed;
  // Base EDID DTD fields are12bit and may also lower their refresh to fit the
  // DTD pixel-clock field. They are not the current monitor's requested mode:
  // the pinned driver's target callback supplies its full DisplayDescriptor,
  // and monitor-description parsing uses registered metadata for the same EDID.
  // This exercises those actual pure SDK builders, not IddCx/CCD or a driver DLL.
  for(const auto& [w,h]:std::array<std::pair<uint32_t,uint32_t>,3>{{{5120,1440},{5120,2880},{7680,4320}}}){
    check(ownedOutputModeSupported(w,h));
    api::EdidOptions options;options.width=w;options.height=h;options.refresh_rate_millihz=60000;
    const auto edid=api::create_edid(options);check(api::has_valid_edid_checksums(edid));
    const auto timing=api::read_preferred_timing(edid);
    check(timing.horizontal_active==(std::min)(w,4095u)&&timing.vertical_active==(std::min)(h,4095u));
    check(timing.horizontal_active!=w||timing.vertical_active!=h);
    const uint32_t totalWidth=timing.horizontal_active+timing.horizontal_blanking,totalHeight=timing.vertical_active+timing.vertical_blanking;
    const uint64_t edidClock=uint64_t(timing.pixel_clock_10khz)*10000;
    const api::WindowsDriverModeShape fallback{timing.horizontal_active,timing.vertical_active,totalWidth,totalHeight,edidClock,uint32_t(edidClock*1000/(uint64_t(totalWidth)*totalHeight))};
    const auto requested=api::active_windows_driver_mode_shape(w,h,60000);
    check(requested.width==w&&requested.height==h&&requested.total_width==w&&requested.total_height==h&&requested.pixel_rate==uint64_t(w)*h*60);
    const auto [modes,preferred]=api::build_windows_driver_target_mode_shapes(fallback,&requested);
    check(preferred<modes.size()&&modes[preferred].width==w&&modes[preferred].height==h&&modes[preferred].refresh_rate_millihz==60000&&modes[preferred].pixel_rate==requested.pixel_rate);
    const auto frequencies=api::windows_driver_signal_frequencies(modes[preferred]);
    check(frequencies.vertical.denominator&&uint64_t(frequencies.vertical.numerator)==60ull*frequencies.vertical.denominator);
    api::WindowsDriverRegisteredModeStore registry;check(registry.register_mode(edid,requested));
    const auto registered=registry.registered_mode(edid);check(registered&&registered->width==w&&registered->height==h);
    const auto [monitorModes,monitorPreferred]=api::build_windows_driver_mode_shapes(registered);
    check(monitorPreferred<monitorModes.size()&&monitorModes[monitorPreferred].width==w&&monitorModes[monitorPreferred].height==h&&monitorModes[monitorPreferred].pixel_rate==requested.pixel_rate);
    registry.unregister_mode(edid);check(!registry.registered_mode(edid));
    ++passed;
  }
  std::cout<<"{\"passed\":"<<passed<<",\"actualUpstreamLeaseStore\":true,\"simulatedAsyncDeparture\":true,\"unchangedNeighborTargetPreserved\":true,\"globalLeaseReleaseDuringReconfigure\":false,\"transport\":\"memory fixture\",\"osDriverCalls\":0,\"captureReadinessClaimed\":false,\"largeModeMetadataGroups\":3,\"actualSignedDriverLoaded\":false,\"actualCcdModeApplied\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
