#include "machine.hpp"
#include "native_display_contract.hpp"
#include "native_display_activation.hpp"
#include <cstring>
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
NativeCcdData capturedForeignWithOwned(){
  NativeCcdData data;data.modes.resize(11);data.paths.resize(2);
  // Exact active NVIDIA reference/layout observed on Code245; no private data.
  auto& source=data.modes[4];source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId={32198,0};source.id=0;
  source.sourceMode={2560,1600,DISPLAYCONFIG_PIXELFORMAT_32BPP,{1280,0}};
  auto& target=data.modes[3];target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId={32198,0};target.id=4352;
  auto& foreign=data.paths[0];foreign.flags=DISPLAYCONFIG_PATH_ACTIVE;foreign.sourceInfo.adapterId=foreign.targetInfo.adapterId={32198,0};foreign.sourceInfo.id=0;foreign.targetInfo.id=4352;
  foreign.sourceInfo.modeInfoIdx=327679;foreign.targetInfo.modeInfoIdx=262143;foreign.targetInfo.targetAvailable=TRUE;
  foreign.targetInfo.outputTechnology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI;foreign.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;foreign.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;foreign.targetInfo.refreshRate={59860,1000};
  // One independent owned output alongside that captured foreign path.
  auto& ownSource=data.modes[0];ownSource.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;ownSource.adapterId={77,0};ownSource.id=1;ownSource.sourceMode={1280,800,DISPLAYCONFIG_PIXELFORMAT_32BPP,{0,0}};
  auto& ownTarget=data.modes[1];ownTarget.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;ownTarget.adapterId={77,0};ownTarget.id=2;
  auto& owned=data.paths[1];owned.flags=DISPLAYCONFIG_PATH_ACTIVE;owned.sourceInfo.adapterId=owned.targetInfo.adapterId={77,0};owned.sourceInfo.id=1;owned.targetInfo.id=2;owned.sourceInfo.modeInfoIdx=0;owned.targetInfo.modeInfoIdx=1;owned.targetInfo.targetAvailable=TRUE;
  return data;
}
Json lease(){return JsonParser::parse(R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","servicePid":441,"generation":3,"deviceInstanceId":"ROOT\\DISPLAY\\0001","leaseActive":true,"configurationPending":false,"activationVerified":false,"recoveryRequired":false,"targets":[{"id":"0","adapterLow":77,"adapterHigh":0,"target":2,"width":1280,"height":800}]})");}
auto sourceName=[](const DISPLAYCONFIG_PATH_INFO& path){return L"\\\\.\\DISPLAY"+std::to_wstring(path.sourceInfo.id+1);};
DisplayState readiness(const NativeCcdData& data){
  DisplayState state;state.installed=state.deviceReady=state.owned=true;state.foreign=0;state.configurationPending=true;
  try{state.monitors=nativeDisplayMonitors(nativeDisplayTargets(lease()),data.paths,data.modes,sourceName);
    const auto& first=state.monitors.front();state.active=true;state.name=first.name;state.width=first.width;state.height=first.height;state.left=first.left;state.top=first.top;state.error="native_display_activation_pending";
  }catch(...){state.error="native_display_topology_mismatch";}
  return state;
}
bool rejected(const NativeCcdData& data){try{(void)nativeDisplayMonitors(nativeDisplayTargets(lease()),data.paths,data.modes,sourceName);return false;}catch(const std::runtime_error& e){return std::string(e.what())=="native_display_topology_mismatch";}}
}
int main(){try{unsigned passed=0;
  const auto captured=capturedForeignWithOwned();const auto original=captured;
  require(readiness(captured).monitors.size()==1,"Captured foreign packed references blocked owned readiness");++passed;
  require(std::memcmp(captured.paths.data(),original.paths.data(),captured.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO))==0&&std::memcmp(captured.modes.data(),original.modes.data(),captured.modes.size()*sizeof(DISPLAYCONFIG_MODE_INFO))==0,"Readiness changed raw caller bytes");++passed;
  unsigned activationCalls=0;
  NativeBootstrapAccess access{[]{return true;},[]{return true;},[&]{return readiness(captured);},lease,[&](std::span<const NativeDisplayTarget>,const std::string&){++activationCalls;NativeTopologyResult result;result.ok=true;result.reason.clear();return result;}};
  const auto result=bootstrapNativeDisplays(access);
  require(activationCalls==1&&!result.applyAttempted&&result.display.ready()&&result.diagnostic&&result.diagnostic->windowsVerified&&result.diagnostic->reason.empty(),"Matching activation ACK could not complete actual readiness contract");++passed;
  // Display manager v2 at rest: the rest step leaves the owned output off beside
  // another display (standby); readiness completes without activation.
  {unsigned activations=0,rests=0;
    const auto standby=[&]{auto state=readiness(captured);state.active=false;state.standby=true;state.name.clear();for(auto& monitor:state.monitors)monitor.name.clear();return state;};
    NativeBootstrapAccess rest{[]{return true;},[]{return true;},standby,lease,
      [&](std::span<const NativeDisplayTarget>,const std::string&){++activations;NativeTopologyResult value;value.ok=true;value.reason.clear();return value;},
      [&](std::span<const NativeDisplayTarget>,const std::string&)->std::optional<NativeTopologyResult>{++rests;NativeTopologyResult value;value.ok=true;value.applyAttempted=true;value.reason.clear();return value;}};
    const auto rested=bootstrapNativeDisplays(rest);
    require(rests==1&&!activations&&rested.applyAttempted&&rested.display.standby&&rested.display.streamable()&&!rested.display.ready()&&!rested.display.configurationPending&&
      rested.diagnostic&&rested.diagnostic->windowsVerified,"Standby at rest did not complete readiness without activation");++passed;}
  // Nothing else on (nullopt), or a rest that did not end in standby: activation as before.
  for(const bool settled:{false,true}){unsigned activations=0;
    NativeBootstrapAccess fallback{[]{return true;},[]{return true;},[&]{return readiness(captured);},lease,
      [&](std::span<const NativeDisplayTarget>,const std::string&){++activations;NativeTopologyResult value;value.ok=true;value.reason.clear();return value;},
      [&](std::span<const NativeDisplayTarget>,const std::string&)->std::optional<NativeTopologyResult>{if(!settled)return std::nullopt;NativeTopologyResult value;value.ok=true;value.reason.clear();return value;}};
    const auto activated=bootstrapNativeDisplays(fallback);
    require(activations==1&&activated.display.ready()&&!activated.display.standby,"Without standby the owned output was not activated as before");++passed;}
  for(unsigned scenario=0;scenario<13;++scenario){auto changed=captured;
    switch(scenario){
      case 0:changed.paths[0].sourceInfo.modeInfoIdx=(4u<<16)|65534;break;
      case 1:changed.paths[0].targetInfo.modeInfoIdx=(3u<<16)|65534;break;
      case 2:changed.modes[4].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;break;
      case 3:changed.modes[3].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;break;
      case 4:changed.modes[4].id++;break;
      case 5:changed.modes[3].adapterId.LowPart++;break;
      case 6:changed.paths[0].sourceInfo.modeInfoIdx=(12u<<16)|65535;break;
      case 7:changed.modes[4].sourceMode.position.x=1279;break;
      case 8:changed.modes[0].sourceMode.width=1279;break;
      case 9:changed.modes[0].id++;break;
      case 10:changed.paths[1].targetInfo.targetAvailable=FALSE;break;
      case 11:changed.paths.push_back(changed.paths[1]);break;
      case 12:changed.paths[0].sourceInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;break;
    }
    require(rejected(changed),"Malformed/foreign/overlapping path became ready");++passed;
  }
  auto normalized=captured;normalizeNativeCcdModeReferences(normalized);
  require(nativeDisplayMonitors(nativeDisplayTargets(lease()),normalized.paths,normalized.modes,sourceName).size()==1,"Normalized activation CCD no longer maps to readiness");++passed;
  std::cout<<"{\"passed\":"<<passed<<",\"capturedForeignPackedReference\":true,\"fullBootstrapReadiness\":true,\"windowsQueries\":0,\"displayChanges\":0,\"driverCalls\":0,\"inputEvents\":0}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
