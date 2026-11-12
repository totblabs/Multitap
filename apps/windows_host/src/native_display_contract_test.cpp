#include "service.hpp"
#include <setupapi.h>
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
Json snapshot(){return JsonParser::parse(R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","servicePid":441,"tick":10000,"leaseActive":true,"targets":[{"id":"0","adapterLow":33,"adapterHigh":-1,"target":0,"width":1920,"height":1080},{"id":"1","adapterLow":33,"adapterHigh":-1,"target":1,"width":1280,"height":720}]})");}
Json& target(Json& value,size_t index){return std::get<Json::Array>(std::get<Json::Object>(value.value)["targets"].value).at(index);}
void number(Json& value,const char* key,double n){Json child;child.value=n;std::get<Json::Object>(value.value)[key]=child;}
void text(Json& value,const char* key,const std::string& s){Json child;child.value=s;std::get<Json::Object>(value.value)[key]=child;}
}
int main(){try{
  int passed=0;
  require(!nativeProviderSelected(Json{},false)&&nativeProviderSelected(Json{},true)&&nativeProviderSelected(JsonParser::parse(R"({"enabled":"true","provider":"legacy"})"),true)&&!nativeProviderSelected(JsonParser::parse(R"({"enabled":false})"),true)&&nativeProviderSelected(JsonParser::parse(R"({"enabled":true,"provider":"future"})"),true),"Uncertain selected provider fell through to legacy operations");++passed;
  const auto original=snapshot();const auto targets=nativeDisplayTargets(original);
  {
    const NativeRuntimeRecord stable{"ROOT\\DISPLAY\\00a1","stable",8,{{"0",1920,1080}}};
    require(nativeDisplayCleanupRuntimeGeneration(stable.instance,7,nativeRuntimeBytes(stable))==8&&nativeDisplayCleanupRuntimeGeneration(stable.instance,8,nativeRuntimeBytes(stable))==8,"Verified same-owner runtime generation was not admitted for cleanup");++passed;
    for(int scenario=0;scenario<7;++scenario){
      auto runtime=stable;auto instance=stable.instance;uint32_t held=7;std::optional<std::string> bytes;
      switch(scenario){case 0:runtime.generation=6;break;case 1:instance="ROOT\\DISPLAY\\00b2";break;case 2:runtime.phase="changing";break;case 3:runtime.phase="recovery";break;case 4:held=0;break;case 5:break;case 6:break;}
      if(scenario!=5)bytes=scenario==6?std::string("malformed"):nativeRuntimeBytes(runtime);
      bool refused=false;try{nativeDisplayCleanupRuntimeGeneration(instance,held,bytes);}catch(...){refused=true;}
      require(refused,"Cleanup admitted a missing, stale, foreign or non-stable runtime receipt");++passed;
    }
  }
  {
    const auto config=JsonParser::parse(R"({"provider":"libvirtualdisplay-1.6.4","enabled":true,"deviceInstanceId":"ROOT\\DISPLAY\\00a1"})");
    auto state=original;text(state,"deviceInstanceId","ROOT\\DISPLAY\\00a1");number(state,"generation",7);
    {const auto held=nativeDisplayHeldLease(state);require(held.instance=="ROOT\\DISPLAY\\00a1"&&held.generation==7&&held.targets.size()==2,"Validated held lease binding rejected");++passed;}
    for(int scenario=0;scenario<7;++scenario){auto value=state;std::pair<std::string,uint32_t> binding;
      switch(scenario){case 0:number(value,"generation",0);break;case 1:number(value,"generation",-1);break;case 2:number(value,"generation",7.5);break;case 3:number(value,"generation",4294967296.0);break;case 4:number(target(value,0),"width",0);break;case 5:text(value,"deviceInstanceId","foreign-device");break;case 6:std::get<Json::Object>(value.value).erase("deviceInstanceId");break;}
      bool refused=false;try{const auto held=nativeDisplayHeldLease(value);binding={held.instance,held.generation};}catch(...){refused=true;}
      require(refused&&binding.first.empty()&&!binding.second,"Malformed normal snapshot seeded a cleanup binding");++passed;
    }
    require(nativeDisplayCleanupSnapshotAllowed(config,state,SERVICE_RUNNING,441,441,"ROOT\\DISPLAY\\00a1",7,15000)&&nativeDisplayCleanupSnapshotAllowed(config,state,SERVICE_STOP_PENDING,441,441,"ROOT\\DISPLAY\\00a1",7,15000),"Exact held lease cleanup rejected");++passed;
    for(int scenario=0;scenario<11;++scenario){auto current=state;auto settings=config;DWORD serviceState=SERVICE_STOP_PENDING,pid=441;uint64_t now=15000;
      switch(scenario){case 0:serviceState=SERVICE_STOPPED;break;case 1:serviceState=SERVICE_START_PENDING;break;case 2:pid=442;break;case 3:number(current,"generation",8);break;case 4:number(current,"generation",7.5);break;case 5:text(current,"deviceInstanceId","ROOT\\DISPLAY\\00b2");break;case 6:now=15001;break;case 7:std::get<Json::Object>(current.value)["leaseActive"].value=false;break;case 8:std::get<Json::Object>(current.value)["configurationPending"].value=true;break;case 9:std::get<Json::Object>(current.value)["recoveryRequired"].value=true;break;case 10:std::get<Json::Object>(settings.value)["enabled"].value=false;break;}
      require(!nativeDisplayCleanupSnapshotAllowed(settings,current,serviceState,pid,441,"ROOT\\DISPLAY\\00a1",7,now),"Cleanup admitted stale/replaced/unowned lease");++passed;
    }
  }
  require(targets.size()==2&&targets[0].id=="0"&&targets[1].id=="1"&&targets[0].adapter.HighPart==-1&&targets[0].target==0,"Valid driver target zero/signed LUID rejected");++passed;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(2);std::vector<DISPLAYCONFIG_MODE_INFO> modes(2);
  for(size_t i=0;i<2;++i){auto& path=paths[i];path.flags=DISPLAYCONFIG_PATH_ACTIVE|(i?0:DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE);path.targetInfo.targetAvailable=TRUE;path.targetInfo.adapterId=targets[i].adapter;path.targetInfo.id=targets[i].target;path.sourceInfo.adapterId=targets[i].adapter;path.sourceInfo.id=UINT32(i);
    if(!i){path.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;path.sourceInfo.sourceModeInfoIdx=UINT32(i);}else path.sourceInfo.modeInfoIdx=UINT32(i);
    auto& mode=modes[i];mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.id=UINT32(i);mode.adapterId=targets[i].adapter;mode.sourceMode.width=targets[i].width;mode.sourceMode.height=targets[i].height;mode.sourceMode.position={LONG(i*1920),0};}
  const auto name=[](const auto& path){return L"\\\\.\\DISPLAY"+std::to_wstring(path.sourceInfo.id+2);};
  require(nativeDisplayMonitors(targets,paths,modes,name).size()==2,"Actual virtual/legacy CCD source indices did not map");++passed;
  {auto mixed=paths;auto data=modes;auto foreign=paths[0];foreign.targetInfo.adapterId.LowPart=77;foreign.sourceInfo.adapterId.LowPart=77;foreign.sourceInfo.id=5;foreign.sourceInfo.sourceModeInfoIdx=2;
    auto mode=modes[0];mode.adapterId=foreign.sourceInfo.adapterId;mode.id=5;mode.sourceMode.position={3200,0};mode.sourceMode.width=800;data.push_back(mode);mixed.push_back(foreign);auto inactive=paths[0];inactive.flags&=~DISPLAYCONFIG_PATH_ACTIVE;mixed.push_back(inactive);require(nativeDisplayMonitors(targets,mixed,data,name).size()==2,"Unrelated foreign/inactive path displaced an owned path");++passed;}
  for(const bool malformed:{false,true}){auto mixed=paths;auto foreign=paths[0];foreign.targetInfo.adapterId.LowPart++;if(malformed)foreign.sourceInfo.sourceModeInfoIdx=200;mixed.push_back(foreign);bool rejected=false;try{nativeDisplayMonitors(targets,mixed,modes,name);}catch(...){rejected=true;}require(rejected,"Console/foreign clone or unknown foreign source was offered as an independent output");++passed;}
  for(int scenario=0;scenario<10;++scenario){auto changed=paths;auto data=modes;auto resolver=std::function<std::wstring(const DISPLAYCONFIG_PATH_INFO&)>(name);
    switch(scenario){
      case 0:changed.push_back(paths[0]);break;
      case 1:changed[1].targetInfo.targetAvailable=FALSE;break;
      case 2:data[1].sourceMode.width=640;break;
      case 3:data[1].sourceMode.position.x=100;break;
      case 4:data[1].id=9;break;
      case 5:data[1].adapterId.HighPart=0;break;
      case 6:data[1].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;break;
      case 7:changed[0].sourceInfo.sourceModeInfoIdx=DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;break;
      case 8:resolver=[](const auto&){return std::wstring(L"\\\\.\\DISPLAY2");};break;
      case 9:resolver=[](const auto&){return std::wstring(L"console");};break;
    }
    bool rejected=false;try{nativeDisplayMonitors(targets,changed,data,resolver);}catch(...){rejected=true;}require(rejected,"Unproven/ambiguous/overlapping native CCD output accepted");++passed;
  }
  // Pass the native public IDs through the actual worker/service envelope,
  // the path that rejected the previous native-<64-bit ID> representation.
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.name=L"\\\\.\\DISPLAY2";display.width=3200;display.height=1080;
  for(size_t i=0;i<targets.size();++i)display.monitors.push_back({targets[i].id,L"\\\\.\\DISPLAY"+std::to_wstring(i+2),LONG(i*1920),0,targets[i].width,targets[i].height,false});
  const auto roundTrip=parseDisplayState(JsonParser::parse(workerDisplayJson(display)));
  require(roundTrip.ready()&&roundTrip.monitors.size()==2&&roundTrip.monitors[0].id=="0"&&roundTrip.monitors[1].id=="1","Native outputs lost in real worker envelope");++passed;
  const auto trial=JsonParser::parse(R"({"provider":"libvirtualdisplay-1.6.4","enabled":true,"viewportTrialEnabled":true})");
  require(nativeViewportTrialReady(roundTrip,trial,true),"Ready explicit native trial cannot advertise viewport changes");++passed;
  for(int scenario=0;scenario<11;++scenario){auto state=roundTrip;auto config=trial;bool interactive=true;
    switch(scenario){
      case 0:state.active=false;break;
      case 1:state.configurationPending=true;break;
      case 2:state.recoveryRequired=true;break;
      case 3:state.rebootRequired=true;break;
      case 4:state.remoteSession=true;break;
      case 5:state.deviceReady=false;break;
      case 6:state.monitors.clear();break;
      case 7:interactive=false;break;
      case 8:config=JsonParser::parse(R"({"provider":"libvirtualdisplay-1.6.4","enabled":true})");break;
      case 9:text(config,"viewportTrialEnabled","true");break;
      case 10:text(config,"provider","legacy-mtt");break;
    }
    require(!nativeViewportTrialReady(state,config,interactive),"Unready or unselected native trial advertised viewport changes");++passed;
  }
  for(const auto& monitor:roundTrip.monitors){
    auto request=JsonParser::parse(R"({"sessionId":"00000000-0000-0000-0000-000000000001","requestId":"native-monitor-qa","resizeId":"00000000-0000-0000-0000-000000000002","monitor":"0","width":1600,"height":900,"fps":60,"epoch":1})");
    text(request,"monitor",monitor.id);require(streamResizeRequest(request).monitor==monitor.id,"A native public monitor cannot pass the actual resize protocol");++passed;
  }
  for(int scenario=0;scenario<18;++scenario){auto value=snapshot();
    switch(scenario){
      case 0:text(target(value,0),"id","native-12345678901234567890");break;
      case 1:text(target(value,1),"id","0");break;
      case 2:number(target(value,1),"target",0);break;
      case 3:number(target(value,0),"width",1919);break;
      case 4:number(target(value,0),"height",4294967296.0);break;
      case 5:number(target(value,0),"adapterLow",4294967296.0);break;
      case 6:number(target(value,0),"adapterHigh",2147483648.0);break;
      case 7:number(target(value,0),"target",-1);break;
      case 8:number(target(value,0),"width",640.5);break;
      case 9:std::get<Json::Array>(std::get<Json::Object>(value.value)["targets"].value).clear();break;
      case 10:std::get<Json::Array>(std::get<Json::Object>(value.value)["targets"].value).push_back(target(value,0));break;
      case 11:number(value,"schema",2);break;
      case 12:text(value,"provider","other-driver");break;
      case 13:std::get<Json::Object>(value.value)["leaseActive"].value=false;break;
      case 14:text(target(value,0),"id","1");std::get<Json::Array>(std::get<Json::Object>(value.value)["targets"].value).resize(1);break;
      case 15:text(target(value,0),"id","01");break;
      case 16:number(target(value,0),"height",0);break;
      case 17:std::get<Json::Object>(target(value,0).value).erase("adapterHigh");break;
    }
    bool rejected=false;try{nativeDisplayTargets(value);}catch(...){rejected=true;}
    require(rejected,"Malformed/stale/foreign native mapping accepted");++passed;
  }
  require(nativeDisplaySnapshotFresh(original,15000,441),"Fresh boundary rejected");++passed;
  require(!nativeDisplaySnapshotFresh(original,15001,441)&&!nativeDisplaySnapshotFresh(original,9999,441)&&!nativeDisplaySnapshotFresh(original,10000,442),"Stale future or different service PID accepted");++passed;
  for(const char* key:{"tick","servicePid"}){auto invalid=original;number(invalid,key,10000.5);require(!nativeDisplaySnapshotFresh(invalid,11000,441),"Fractional service evidence accepted");++passed;}
  std::cout<<"{\"cases\":"<<passed<<",\"realWorkerEnvelopeRoundTrip\":true,\"malformedMappingsRejected\":18,\"pnpAbi\":{\"Device\":"<<sizeof(SP_DEVINFO_DATA)<<",\"Parameters\":"<<sizeof(SP_DEVINSTALL_PARAMS_W)<<",\"Driver\":"<<sizeof(SP_DRVINFO_DATA_V2_W)<<"},\"driverCalls\":0,\"serviceOrDeviceChanged\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
