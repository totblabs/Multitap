#include "native_render_adapter.hpp"
#include "runtime.hpp"
#include <virtual_display/driver/ioctl_dispatcher.h>
#include <iostream>
#include <source_location>
using namespace host;
namespace {
namespace api=virtual_display::driver;
unsigned checks=0;
void require(bool ok,const std::source_location at=std::source_location::current()){++checks;if(!ok)throw std::runtime_error("render preference check "+std::to_string(at.line()));}
constexpr uint64_t selected=0xfedcba9801234567ull,lease=api::kMinOpaqueLeaseId+101;
struct Backend:api::DisplayDriverBackend {
  api::SetRenderAdapterRequest request{};unsigned renders=0,arrivals=0;bool deny=false;std::vector<std::string> order;
  api::BackendDisplayResult arrive_temporary_display(const api::DisplayDescriptor&)override{++arrivals;order.push_back("arrive");return {api::BackendError::None,{44,-1},arrivals};}
  api::BackendError depart_temporary_display(uint64_t,uint64_t)override{return api::BackendError::None;}
  api::BackendError set_permanent_display_count(const api::PermanentDisplayCountRequest&)override{return api::BackendError::None;}
  api::BackendError apply_display_manifest(const api::DisplayManifest&)override{return api::BackendError::None;}
  api::BackendError set_render_adapter(const api::SetRenderAdapterRequest& value)override{++renders;request=value;order.push_back("render");return deny?api::BackendError::Failed:api::BackendError::None;}
};
struct Wire:api::ControlTransport {
  Backend backend;api::DriverController controller{api::DisplayStore{2,8},backend};api::IoctlDispatcher dispatcher{controller};
  unsigned stateQueries=0;int failQuery=-1;bool badProtocol=false,badNamespace=false,invalidState=false;size_t renderBytes=0;
  bool ioctl(uint32_t code,const void* in,size_t bytes,void* out,size_t capacity,size_t& returned,uint32_t& error)override{
    returned=0;error=0;if(code==api::kIoctlQueryDisplayState&&int(++stateQueries)==failQuery){error=123;return false;}
    if(code==api::kIoctlSetRenderAdapter)renderBytes=bytes;
    const auto r=dispatcher.dispatch(code,in,bytes,out,capacity,std::chrono::steady_clock::time_point{std::chrono::milliseconds(1000)});
    returned=r.bytes_returned;error=r.status==api::IoctlStatus::Success?0:5;
    if(!error&&code==api::kIoctlGetProtocolVersion&&badProtocol)++static_cast<api::ProtocolVersion*>(out)->major;
    if(!error&&code==api::kIoctlQueryDisplayState){auto& state=*static_cast<api::QueryDisplayStateResult*>(out);if(badNamespace)state.api_namespace={};if(invalidState)state.reserved=1;}
    return !error;
  }
};
api::OwnerCapability owner(){api::OwnerCapability value{};value.bytes.fill(42);return value;}
void createForeign(api::ControlClient& client){api::CreateTemporaryDisplayOwnedRequest request{};request.owner_capability=owner();request.display.lease_id=lease+1;request.display.display_id=999;request.display.width=1920;request.display.height=1080;request.display.refresh_rate_millihz=60000;std::memcpy(request.display.display_name,"Foreign",8);require(client.create_temporary_display_owned(request).ok());}
void predicatesAndMarshal(){
  Wire wire;api::ControlClient client(wire);require(nativeRenderOutputsAbsent(client.query_display_state().value));
  unsigned choices=0;const auto result=preferNativeRenderAdapter(client,[&]()->std::optional<uint64_t>{++choices;return selected;});
  require(std::string(result.status)=="accepted"&&result.requested==selected&&choices==1&&wire.backend.renders==1&&wire.renderBytes==sizeof(api::SetRenderAdapterRequest));
  const auto& sent=wire.backend.request;require(api::is_valid_api_namespace(sent.api_namespace)&&!sent.flags&&!sent.reserved&&sent.adapter_luid.low_part==uint32_t(selected&0xffffffffull)&&uint32_t(sent.adapter_luid.high_part)==uint32_t(selected>>32));
  const auto json=JsonParser::parse(nativeRenderPreferenceJson(result));
  for(const auto* key:{"actualVerified","actualRenderAdapterLuid","warp"})require(std::holds_alternative<std::nullptr_t>(json.get(key).value));
  require(json.get("requestedAdapterLuid").string()==gpu::adapterLuidText(selected));
  auto state=client.query_display_state().value;state.api_namespace={};require(!nativeRenderOutputsAbsent(state));
  state=client.query_display_state().value;state.reserved=1;require(!nativeRenderOutputsAbsent(state));
}
void activeAndForeignGuards(){
  for(const bool permanent:{false,true}){Wire wire;api::ControlClient client(wire);if(permanent){api::PermanentDisplayCountRequest request{};request.display_count=1;require(client.set_permanent_display_count(request).ok());}else createForeign(client);
    const auto before=client.query_display_state().value;unsigned choices=0;const auto result=preferNativeRenderAdapter(client,[&]()->std::optional<uint64_t>{++choices;return selected;});const auto after=client.query_display_state().value;
    require(std::string(result.status)=="skipped"&&std::string(result.reason)=="outputs_present"&&choices==0&&!wire.backend.renders);
    require(std::memcmp(&before,&after,sizeof(before))==0);
  }
  Wire wire;api::ControlClient client(wire);const auto result=preferNativeRenderAdapter(client,[&]()->std::optional<uint64_t>{createForeign(client);return selected;});
  require(std::string(result.status)=="skipped"&&!wire.backend.renders&&wire.backend.arrivals==1&&wire.stateQueries==2);
}
void failuresAndFallback(){
  for(int kind=0;kind<6;kind++){Wire wire;api::ControlClient client(wire);if(kind==0)wire.badProtocol=true;if(kind==1)wire.badNamespace=true;if(kind==2)wire.invalidState=true;if(kind==3)wire.failQuery=1;if(kind==4)wire.failQuery=2;if(kind==5)wire.backend.deny=true;
    const auto result=preferNativeRenderAdapter(client,[]()->std::optional<uint64_t>{return selected;});require(std::string(result.status)=="failed");require(wire.backend.renders==unsigned(kind==5));
  }
  {Wire wire;api::ControlClient client(wire);const auto result=preferNativeRenderAdapter(client,[]()->std::optional<uint64_t>{return {};});require(std::string(result.status)=="skipped"&&!wire.backend.renders);}
  {Wire wire;api::ControlClient client(wire);const auto result=preferNativeRenderAdapter(client,[]()->std::optional<uint64_t>{throw std::runtime_error("private error");});require(std::string(result.status)=="failed"&&!wire.backend.renders&&nativeRenderPreferenceJson(result).find("private error")==std::string::npos);}
  for(const bool fail:{false,true}){Wire wire;api::ControlClient client(wire);wire.backend.deny=fail;const auto preferred=preferNativeRenderAdapter(client,[]()->std::optional<uint64_t>{return selected;});
    multitap::vdisplay::Controller controller(client,lease,owner());const multitap::vdisplay::Output output{71,1920,1080};require(controller.start({&output,1})&&controller.state().leaseActive);
    require(std::string(preferred.status)==(fail?"failed":"accepted")&&wire.backend.order==std::vector<std::string>{"render","arrive"});
    const auto again=preferNativeRenderAdapter(client,[]()->std::optional<uint64_t>{return selected;});require(std::string(again.status)=="skipped"&&wire.backend.renders==1);
  }
}
void candidates(){
  const std::vector<gpu::Adapter> adapters{{1,0x10de,1,"not a benchmark"},{2,0x1002,2,"not a benchmark"},{3,0x8086,3,"not a benchmark"},{4,0x15ad,4,"not a benchmark"}};
  std::vector<uint64_t> opened;
  require(chooseNativeRenderAdapter(adapters,[&](const gpu::Adapter& a)->std::optional<uint64_t>{opened.push_back(a.luid);return a.luid;})==1&&opened==std::vector<uint64_t>{1});
  opened.clear();require(chooseNativeRenderAdapter(adapters,[&](const gpu::Adapter& a)->std::optional<uint64_t>{opened.push_back(a.luid);return a.luid==1?99:a.luid;})==2&&opened==std::vector<uint64_t>({1,2}));
  opened.clear();require(!chooseNativeRenderAdapter(adapters,[&](const gpu::Adapter& a)->std::optional<uint64_t>{opened.push_back(a.luid);throw std::runtime_error("probe failed");})&&opened==std::vector<uint64_t>({1,2,3}));
  unsigned calls=0;const auto probe=[&](const gpu::Adapter& a)->std::optional<uint64_t>{++calls;return a.luid;};
  require(!chooseNativeRenderAdapter({},probe)&&!calls);auto invalid=adapters;invalid[1].luid=1;require(!chooseNativeRenderAdapter(invalid,probe)&&!calls);invalid[1].luid=0;require(!chooseNativeRenderAdapter(invalid,probe)&&!calls);invalid.assign(257,adapters[0]);require(!chooseNativeRenderAdapter(invalid,probe)&&!calls);
}
void basicRenderDriverInventory(){
  // An actual inventory: these entries were not marked DXGI SOFTWARE.
  std::vector<gpu::Adapter> actual{{0x8567,5140,140,"Microsoft Basic Render Driver"},{0x10783,5140,140,"Microsoft Basic Render Driver"},{0x11726,5140,140,"Microsoft Basic Render Driver"}};
  std::vector<uint64_t> opened;const auto probe=[&](const gpu::Adapter& adapter)->std::optional<uint64_t>{opened.push_back(adapter.luid);return adapter.luid;};
  require(!chooseNativeRenderAdapter(actual,probe)&&opened.empty());
  Wire wire;api::ControlClient client(wire);const auto skipped=preferNativeRenderAdapter(client,[&]{return chooseNativeRenderAdapter(actual,probe);});
  require(std::string(skipped.status)=="skipped"&&std::string(skipped.reason)=="no_usable_hardware_adapter"&&!skipped.requested&&!wire.backend.renders&&opened.empty());
  actual.push_back({0x22222,0x10de,1,"synthetic NVIDIA fourth"});
  require(chooseNativeRenderAdapter(actual,probe)==0x22222&&opened==std::vector<uint64_t>{0x22222});
}
}
int main(){try{predicatesAndMarshal();activeAndForeignGuards();failuresAndFallback();candidates();basicRenderDriverInventory();std::cout<<"{\"kind\":\"native-render-preference\",\"checks\":"<<checks<<",\"passed\":true,\"transport\":\"actual SDK over substituted backend\",\"gpuCalls\":0,\"driverCalls\":0,\"displayChanges\":0,\"actualRenderVerified\":null}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
