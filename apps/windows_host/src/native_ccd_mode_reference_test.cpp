#include "machine.hpp"
#include "native_display_contract.hpp"
#include "native_activation_ccd.hpp"
#include <cstring>
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
NativeCcdData capturedPath(){
  NativeCcdData data;data.modes.resize(11);
  auto& source=data.modes[4];source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId={32198,0};source.id=0;
  source.sourceMode={2560,1600,DISPLAYCONFIG_PIXELFORMAT_32BPP,{1280,0}};
  auto& target=data.modes[3];target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId={32198,0};target.id=4352;
  auto& signal=target.targetMode.targetVideoSignalInfo;signal.pixelRate=268000000;signal.hSyncFreq={268000000,2720};signal.vSyncFreq={59860,1000};
  signal.activeSize={2560,1600};signal.totalSize={2720,1646};signal.videoStandard=255;signal.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  DISPLAYCONFIG_PATH_INFO path{};path.flags=DISPLAYCONFIG_PATH_ACTIVE;path.sourceInfo.adapterId=path.targetInfo.adapterId={32198,0};path.sourceInfo.id=0;path.targetInfo.id=4352;
  path.sourceInfo.modeInfoIdx=327679;path.targetInfo.modeInfoIdx=262143;path.sourceInfo.statusFlags=DISPLAYCONFIG_SOURCE_IN_USE;path.targetInfo.statusFlags=DISPLAYCONFIG_TARGET_IN_USE;
  path.targetInfo.targetAvailable=TRUE;path.targetInfo.outputTechnology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI;path.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;
  path.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;path.targetInfo.refreshRate={59860,1000};path.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  data.paths={path};return data;
}
bool sameBytes(const NativeCcdData& a,const NativeCcdData& b){return a.paths.size()==b.paths.size()&&a.modes.size()==b.modes.size()&&
  (a.paths.empty()||std::memcmp(a.paths.data(),b.paths.data(),a.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO))==0)&&
  (a.modes.empty()||std::memcmp(a.modes.data(),b.modes.data(),a.modes.size()*sizeof(DISPLAYCONFIG_MODE_INFO))==0);}
bool invalid(const NativeCcdData& data){try{(void)nativeActiveSignatures(data);return false;}catch(const std::runtime_error& error){return std::string(error.what())=="native_topology_mode_invalid";}}
NativeCcdQueryAccess query(const NativeCcdData& data,unsigned& calls){
  NativeCcdQueryAccess access;
  access.sizes=[&](UINT32,UINT32* paths,UINT32* modes){*paths=UINT32(data.paths.size());*modes=UINT32(data.modes.size());return LONG(ERROR_SUCCESS);};
  access.read=[&](UINT32,UINT32* paths,DISPLAYCONFIG_PATH_INFO* p,UINT32* modes,DISPLAYCONFIG_MODE_INFO* m){++calls;require(*paths>=data.paths.size()&&*modes>=data.modes.size(),"Fixture buffer too small");std::copy(data.paths.begin(),data.paths.end(),p);std::copy(data.modes.begin(),data.modes.end(),m);*paths=UINT32(data.paths.size());*modes=UINT32(data.modes.size());return LONG(ERROR_SUCCESS);};
  return access;
}
unsigned wireCases(){
  unsigned passed=0;const auto raw=capturedPath();const auto hints=nativeObservedPackedCcdKeys(raw);
  auto canonical=raw;normalizeNativeCcdModeReferences(canonical);
  canonical.modes={raw.modes[4],raw.modes[3]};canonical.paths[0].sourceInfo.modeInfoIdx=0;canonical.paths[0].targetInfo.modeInfoIdx=1;
  {auto wire=canonical;require(hints.size()==1&&encodeNativeCcdWireReferences(wire,hints)==1&&wire.paths[0].sourceInfo.modeInfoIdx==0xffff&&wire.paths[0].targetInfo.modeInfoIdx==0x1ffff,"Actual reindexed plan did not retain the observed wire representation");
    auto expected=canonical;expected.paths[0].sourceInfo.modeInfoIdx=0xffff;expected.paths[0].targetInfo.modeInfoIdx=0x1ffff;
    require(sameBytes(wire,expected)&&wire.paths[0].flags==DISPLAYCONFIG_PATH_ACTIVE,"Wire encoding changed flags, modes or geometry");
    normalizeNativeCcdModeReferences(wire);require(sameBytes(wire,canonical),"Encoded wire did not roundtrip to canonical CCD");++passed;}
  for(unsigned scenario=0;scenario<8;++scenario){auto observed=raw;switch(scenario){
    case 0:observed.paths.clear();break;case 1:observed.paths[0].sourceInfo.id++;break;case 2:observed.paths[0].targetInfo.adapterId.HighPart++;break;
    case 3:observed.modes[4].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;break;case 4:observed.paths[0].flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;break;
    case 5:observed.paths[0].sourceInfo.modeInfoIdx=(4u<<16)|65534;break;case 6:observed.paths.push_back(observed.paths[0]);break;case 7:observed.paths[0].flags=0;break;
  }auto wire=canonical;require(encodeNativeCcdWireReferences(wire,nativeObservedPackedCcdKeys(observed))==0&&sameBytes(wire,canonical),"Unobserved or ambiguous wire quirk was invented");++passed;}
  for(unsigned scenario=0;scenario<5;++scenario){auto plan=canonical;switch(scenario){case 0:plan.paths[0].targetInfo.id++;break;case 1:plan.modes[0].id++;break;case 2:plan.modes[1].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;break;case 3:plan.paths[0].targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;break;case 4:plan.paths[0].flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;break;}
    const auto before=plan;require(encodeNativeCcdWireReferences(plan,hints)==0&&sameBytes(plan,before),"Different target, invalid mode or virtual flag was repacked");++passed;}
  constexpr UINT32 options=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
  // The real three-display failure: a packed legacy NVIDIA head was active
  // before streaming, hidden during it, then restored beside virtual outputs.
  // Windows rejects the same supplied plan with 87 if its wire packing is lost.
  {bool hidden=false;unsigned sent=0;auto configure=nativeCcdWireConfigure([&]{auto value=raw;if(hidden)value.paths[0].flags=0;return value;},[&](const NativeCcdData& wire,UINT32){++sent;return wire.paths[0].sourceInfo.modeInfoIdx==0xffff&&wire.paths[0].targetInfo.modeInfoIdx==0x1ffff?LONG(ERROR_SUCCESS):LONG(ERROR_INVALID_PARAMETER);});
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(canonical,SDC_APPLY|options)==0,"Active packed-head fixture did not validate");
    hidden=true;
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(canonical,SDC_APPLY|options)==0&&sent==4,"Inactive original head lost its positively observed wire encoding (Win32 87)");++passed;
  }
  {unsigned queries=0;std::vector<NativeCcdData> sent;auto configure=nativeCcdWireConfigure([&]{++queries;return raw;},[&](const NativeCcdData& wire,UINT32){sent.push_back(wire);return LONG(ERROR_SUCCESS);});
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(canonical,SDC_APPLY|options)==0&&queries==2&&sent.size()==2&&sameBytes(sent[0],sent[1])&&sent[0].paths[0].sourceInfo.modeInfoIdx==0xffff,"VALIDATE/APPLY did not use identical observed wire bytes");++passed;}
  {unsigned queries=0,calls=0;auto configure=nativeCcdWireConfigure([&]{auto value=raw;if(queries++){normalizeNativeCcdModeReferences(value);}return value;},[&](const NativeCcdData&,UINT32){++calls;return LONG(ERROR_SUCCESS);});
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(canonical,SDC_APPLY|options)==ERROR_RETRY&&calls==1,"Changed fresh wire evidence reached APPLY");++passed;}
  {unsigned calls=0;auto configure=nativeCcdWireConfigure([&]{return raw;},[&](const NativeCcdData&,UINT32){++calls;return LONG(ERROR_INVALID_PARAMETER);});
    require(configure(canonical,SDC_VALIDATE|options)==ERROR_INVALID_PARAMETER&&configure(canonical,SDC_APPLY|options)==ERROR_RETRY&&calls==1,"A rejected validation reached APPLY");++passed;}
  {unsigned calls=0;auto configure=nativeCcdWireConfigure([&]{return raw;},[&](const NativeCcdData&,UINT32){++calls;return LONG(ERROR_SUCCESS);});auto changed=canonical;changed.modes[0].sourceMode.position.x++;
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(changed,SDC_APPLY|options)==ERROR_RETRY&&calls==1,"A different supplied plan reached APPLY");++passed;}
  {unsigned queries=0,calls=0;auto configure=nativeCcdWireConfigure([&]()->NativeCcdData{++queries;throw std::runtime_error("unexpected_raw_query");},[&](const NativeCcdData&,UINT32){++calls;return LONG(ERROR_SUCCESS);});auto pathsOnly=canonical;pathsOnly.modes.clear();
    const UINT32 topology=SDC_TOPOLOGY_SUPPLIED|SDC_VIRTUAL_MODE_AWARE;
    require(configure(pathsOnly,SDC_VALIDATE|topology)==0&&configure(pathsOnly,SDC_APPLY|topology)==0&&queries==0&&calls==2,"Paths-only recovery gained a mode-query dependency");++passed;}
  // Real activation rollback, with only Windows query/configure replaced.
  {auto observed=raw;auto& source=observed.modes[0];source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId={77,0};source.id=1;source.sourceMode={1280,800,DISPLAYCONFIG_PIXELFORMAT_32BPP,{0,0}};
    auto& target=observed.modes[1];target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId={77,0};target.id=2;
    auto owned=observed.paths[0];owned.flags=DISPLAYCONFIG_PATH_ACTIVE|DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;owned.sourceInfo.adapterId=owned.targetInfo.adapterId={77,0};owned.sourceInfo.id=1;owned.targetInfo.id=2;owned.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;owned.sourceInfo.sourceModeInfoIdx=0;owned.targetInfo.targetModeInfoIdx=1;owned.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;owned.targetInfo.refreshRate={60,1};observed.paths.push_back(owned);
    auto current=observed;normalizeNativeCcdModeReferences(current);unsigned applies=0,encoded=0;
    auto configure=nativeCcdWireConfigure([&]{return observed;},[&](const NativeCcdData& wire,UINT32 flags){const auto path=std::find_if(wire.paths.begin(),wire.paths.end(),[](const auto& p){return p.targetInfo.id==4352;});if(path!=wire.paths.end()&&(path->sourceInfo.modeInfoIdx&0xffff)==0xffff)++encoded;if(flags&SDC_APPLY)return ++applies==1?LONG(ERROR_GEN_FAILURE):LONG(ERROR_SUCCESS);return LONG(ERROR_SUCCESS);});
    NativeTopologyAccess access{[&]{return current;},configure,[]{return true;}};const std::array<NativeDisplayTarget,1> desired{{{"0",{77,0},2,1024,800}}};
    const auto result=activateNativeTopology(desired,access);
    if(result.ok||!result.applyAttempted||!result.restoreAttempted||!result.topologyRestored||result.recoveryRequired||applies!=2||encoded!=4)throw std::runtime_error("Activation/rollback wire fixture failed: "+result.reason+" / "+std::to_string(result.systemError)+" / applies="+std::to_string(applies)+" / encoded="+std::to_string(encoded));++passed;
  }
  return passed;
}
unsigned inactiveRestoreCases(){
  unsigned passed=0;const auto raw=capturedPath();auto canonical=raw;normalizeNativeCcdModeReferences(canonical);
  canonical.modes={raw.modes[4],raw.modes[3]};canonical.paths[0].sourceInfo.modeInfoIdx=0;canonical.paths[0].targetInfo.modeInfoIdx=1;
  auto inactive=raw;inactive.paths[0].flags=0;constexpr UINT32 options=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
  const auto windowsLegacy=[](const NativeCcdData& wire,UINT32){return wire.paths[0].sourceInfo.modeInfoIdx==0xffff&&wire.paths[0].targetInfo.modeInfoIdx==0x1ffff?LONG(ERROR_SUCCESS):LONG(ERROR_INVALID_PARAMETER);};
  // Cold service-start recovery can use exact RAW Windows database evidence,
  // even when the legacy head is absent from the ACTIVE query.
  {unsigned databases=0;std::vector<NativeCcdData> sent;
    auto configure=nativeCcdWireConfigure([]{return NativeCcdData{};},[&](const NativeCcdData& wire,UINT32 flags){sent.push_back(wire);return windowsLegacy(wire,flags);},[&]{return inactive;},[&]{++databases;return raw;});
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(canonical,SDC_APPLY|options)==0&&databases==2&&sent.size()==2&&sameBytes(sent[0],sent[1]),"Cold recovery did not re-prove the exact inactive head from RAW database data");
    auto expected=canonical;expected.paths[0].sourceInfo.modeInfoIdx=0xffff;expected.paths[0].targetInfo.modeInfoIdx=0x1ffff;
    require(sameBytes(sent[0],expected),"Inactive-head encoding changed modes, geometry, target identity or flags");++passed;
  }
  for(unsigned scenario=0;scenario<16;++scenario){auto database=raw,available=inactive,active=NativeCcdData{};bool databaseThrows=false,availableThrows=false;
    switch(scenario){
      case 0:database.paths.clear();break;case 1:normalizeNativeCcdModeReferences(database);break;case 2:database.paths[0].targetInfo.adapterId.HighPart++;break;
      case 3:database.paths[0].targetInfo.id++;break;case 4:database.modes[4].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;break;
      case 5:database.paths[0].flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;break;case 6:database.paths[0].flags=0;break;
      case 7:database.paths.push_back(database.paths[0]);break;case 8:available.paths[0].targetInfo.targetAvailable=FALSE;break;
      case 9:available.paths.push_back(available.paths[0]);break;case 10:active=raw;normalizeNativeCcdModeReferences(active);break;
      case 11:active=raw;active.paths[0].flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;break;
      case 12:active=raw;active.paths.push_back(active.paths[0]);break;case 13:databaseThrows=true;break;case 14:availableThrows=true;break;
      case 15:database.paths[0].sourceInfo.id++;break;
    }
    auto configure=nativeCcdWireConfigure([&]{return active;},windowsLegacy,[&]{if(availableThrows)throw std::runtime_error("ALL query unavailable");return available;},[&]{if(databaseThrows)throw std::runtime_error("Database unavailable");return database;});
    require(configure(canonical,SDC_VALIDATE|options)==ERROR_INVALID_PARAMETER,"Unproven, contradictory, unavailable or ambiguous inactive-head evidence was used");++passed;
  }
  // Fresh database/availability changes between VALIDATE and APPLY must never
  // send a differently encoded buffer to the actual apply call.
  for(const bool availabilityChanges:{false,true}){unsigned databases=0,availableCalls=0,sent=0;
    auto configure=nativeCcdWireConfigure([]{return NativeCcdData{};},[&](const NativeCcdData& wire,UINT32 flags){++sent;return windowsLegacy(wire,flags);},[&]{auto value=inactive;if(availableCalls++&&availabilityChanges)value.paths[0].targetInfo.targetAvailable=FALSE;return value;},[&]{auto value=raw;if(databases++&&!availabilityChanges)normalizeNativeCcdModeReferences(value);return value;});
    require(configure(canonical,SDC_VALIDATE|options)==0&&configure(canonical,SDC_APPLY|options)==ERROR_RETRY&&sent==1,"Changed inactive-head evidence reached APPLY");++passed;
  }
  // An earlier packed observation never follows a new LUID/source/target.
  for(unsigned scenario=0;scenario<3;++scenario){bool hidden=false;auto next=canonical,available=inactive;
    switch(scenario){case 0:next.paths[0].targetInfo.adapterId.LowPart++;next.paths[0].sourceInfo.adapterId.LowPart++;next.modes[0].adapterId.LowPart++;next.modes[1].adapterId.LowPart++;available.paths[0].sourceInfo.adapterId.LowPart++;available.paths[0].targetInfo.adapterId.LowPart++;break;
      case 1:next.paths[0].sourceInfo.id++;next.modes[0].id++;available.paths[0].sourceInfo.id++;break;
      case 2:next.paths[0].targetInfo.id++;next.modes[1].id++;available.paths[0].targetInfo.id++;break;}
    auto configure=nativeCcdWireConfigure([&]{return hidden?NativeCcdData{}:raw;},windowsLegacy,[&]{return hidden?available:raw;});
    require(configure(canonical,SDC_VALIDATE|options)==0,"Historical key fixture did not seed RAW evidence");hidden=true;
    require(configure(next,SDC_VALIDATE|options)==ERROR_INVALID_PARAMETER,"An old packed witness followed a rebound identity");++passed;
  }
  // An active contradiction remains newer than the persisted database even
  // after the target is hidden again. Only a new active packed observation
  // can re-enable the earlier representation for this worker.
  for(unsigned scenario=0;scenario<3;++scenario){unsigned phase=0;NativeCcdData last;
    auto configure=nativeCcdWireConfigure([&]{auto value=phase==2||phase==4?inactive:raw;if(phase==1){if(scenario==0)normalizeNativeCcdModeReferences(value);if(scenario==1)value.paths[0].flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;if(scenario==2)value.paths.push_back(value.paths[0]);}return value;},[&](const NativeCcdData& wire,UINT32){last=wire;return LONG(ERROR_SUCCESS);},[&]{return inactive;},[&]{return raw;});
    require(configure(canonical,SDC_VALIDATE|options)==0&&last.paths[0].sourceInfo.modeInfoIdx==0xffff,"Packed witness transition fixture did not seed");
    phase=1;require(configure(canonical,SDC_VALIDATE|options)==0&&sameBytes(last,canonical),"Fresh active contradiction did not replace packed history");
    phase=2;require(configure(canonical,SDC_VALIDATE|options)==0&&sameBytes(last,canonical),"Stale database resurrected a contradicted inactive witness");
    phase=3;require(configure(canonical,SDC_VALIDATE|options)==0&&last.paths[0].sourceInfo.modeInfoIdx==0xffff,"Fresh packed evidence did not replace the negative observation");
    phase=4;require(configure(canonical,SDC_VALIDATE|options)==0&&last.paths[0].sourceInfo.modeInfoIdx==0xffff,"New packed history did not survive the next inactive phase");++passed;
  }
  return passed;
}
}
int main(){try{unsigned passed=0;
  {auto data=capturedPath();require(invalid(data),"Actual unflagged packed references did not reproduce the old failure");auto expected=data;expected.paths[0].sourceInfo.modeInfoIdx=4;expected.paths[0].targetInfo.modeInfoIdx=3;
    require(normalizeNativeCcdModeReferences(data)==1&&sameBytes(data,expected),"Normalization changed more than the two mode references");
    require(data.paths[0].flags==DISPLAYCONFIG_PATH_ACTIVE&&nativeActiveSignatures(data).size()==1,"Legacy flags or active semantics changed");
    require(normalizeNativeCcdModeReferences(data)==0&&sameBytes(data,expected),"Normalization was not idempotent");++passed;}
  for(unsigned scenario=0;scenario<17;++scenario){auto data=capturedPath();auto& path=data.paths[0];switch(scenario){
    case 0:path.flags=0;break;case 1:path.flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;break;
    case 2:path.sourceInfo.modeInfoIdx=4;break;case 3:path.targetInfo.modeInfoIdx=3;break;
    case 4:path.sourceInfo.modeInfoIdx=(4u<<16)|65534;break;case 5:path.targetInfo.modeInfoIdx=(3u<<16)|65534;break;
    case 6:path.sourceInfo.modeInfoIdx=(12u<<16)|65535;break;case 7:path.targetInfo.modeInfoIdx=(12u<<16)|65535;break;
    case 8:data.modes[4].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;break;case 9:data.modes[3].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;break;
    case 10:data.modes[4].id++;break;case 11:data.modes[3].id++;break;case 12:data.modes[4].adapterId.LowPart++;break;
    case 13:data.modes[3].adapterId.LowPart++;break;case 14:data.modes[4].adapterId.HighPart++;break;case 15:data.modes[3].adapterId.HighPart++;break;
    case 16:path.sourceInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;break;
   }const auto before=data;require(normalizeNativeCcdModeReferences(data)==0&&sameBytes(data,before),"Unproven references were normalized");
   if(scenario>=2)require(invalid(data),"An invalid legacy reference no longer failed closed");++passed;
  }
  {auto data=capturedPath();data.paths[0].sourceInfo.modeInfoIdx=4;data.paths[0].targetInfo.modeInfoIdx=3;const auto before=data;
    require(normalizeNativeCcdModeReferences(data)==0&&sameBytes(data,before)&&nativeActiveSignatures(data).size()==1,"Valid legacy references changed");++passed;}
  {auto data=capturedPath();data.paths[0].flags|=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;const auto before=data;
    require(normalizeNativeCcdModeReferences(data)==0&&sameBytes(data,before)&&nativeActiveSignatures(data).size()==1,"Valid virtual references changed");++passed;}
  {const auto data=capturedPath();unsigned calls=0;const auto value=queryNativeCcd(query(data,calls));require(calls==1&&nativeSourceIndex(value.paths[0])==4&&nativeTargetIndex(value.paths[0])==3&&nativeActiveSignatures(value).size()==1,"Shared query did not normalize before consumers");++passed;}
  {const auto data=capturedPath();unsigned calls=0;const auto value=queryNativeActivationCcd(query(data,calls));require(calls==3&&nativeActiveSignatures(value).size()==1&&value.paths[0].flags==DISPLAYCONFIG_PATH_ACTIVE,"Coherent bootstrap did not consume shared normalized views");++passed;}
  passed+=wireCases();
  passed+=inactiveRestoreCases();
  std::cout<<"{\"passed\":"<<passed<<",\"queryCallbacks\":\"substituted\",\"windowsQueries\":0,\"displayChanges\":0,\"driverCalls\":0,\"inputEvents\":0}\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
