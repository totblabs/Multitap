#pragma once
#include "native_display_change.hpp"
#include "native_display_recovery_identity.hpp"
#include <charconv>
namespace host {
inline constexpr size_t nativeBaselineByteLimit=60000;
struct NativeRecoveryBaseline {std::string instance;uint32_t generation=0;NativeTopologyBaseline topology;std::optional<NativeRecoveryScope> scope;};
inline int64_t nativeBaselineInteger(const Json& value,int64_t low,int64_t high){
  const auto* text=std::get_if<std::string>(&value.value);int64_t result=0;if(!text||text->empty()||text->size()>20)throw std::runtime_error("native_baseline_invalid");const auto parsed=std::from_chars(text->data(),text->data()+text->size(),result);
  if(parsed.ec!=std::errc{}||parsed.ptr!=text->data()+text->size()||result<low||result>high||std::to_string(result)!=*text)throw std::runtime_error("native_baseline_invalid");return result;
}
inline std::vector<int64_t> nativeBaselinePathRow(const NativeCcdData& ccd,const DISPLAYCONFIG_PATH_INFO& path){
  auto row=nativePathSignature(ccd,path);row.push_back(path.sourceInfo.statusFlags);row.push_back(path.targetInfo.statusFlags);row.push_back(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?int64_t(path.sourceInfo.cloneGroupId):-1);return row;
}
inline NativeRecoveryScope parseNativeRecoveryScope(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);const auto* bindings=std::get_if<Json::Array>(&value.get("foreign").value);const auto session=value.get("session").number();if(!fields||fields->size()!=4||!bindings||bindings->size()>64||!nativeDisplayInteger(session,1,UINT32_MAX-1))throw std::runtime_error("native_recovery_scope_invalid");
  NativeRecoveryScope scope{DWORD(session),value.get("logon").string(),uint64_t(nativeBaselineInteger(value.get("tick"),1,INT64_MAX)),{}};
  for(const auto& item:*bindings){const auto* object=std::get_if<Json::Object>(&item.value);if(!object||object->size()!=11||!std::holds_alternative<bool>(item.get("complete").value))throw std::runtime_error("native_recovery_scope_invalid");const auto u32=[&](const char* key){const auto n=item.get(key).number(-1);if(!nativeDisplayInteger(n,0,UINT32_MAX))throw std::runtime_error("native_recovery_scope_invalid");return DWORD(n);};const auto i32=[&](const char* key){const auto n=item.get(key).number(-2147483649.0);if(!nativeDisplayInteger(n,INT32_MIN,INT32_MAX))throw std::runtime_error("native_recovery_scope_invalid");return LONG(n);};
    for(const auto name:{"sourcePath","targetPath","monitorPath"})if(!std::holds_alternative<std::string>(item.get(name).value))throw std::runtime_error("native_recovery_scope_invalid");scope.foreign.push_back({{u32("sourceAdapterLow"),i32("sourceAdapterHigh")},{u32("targetAdapterLow"),i32("targetAdapterHigh")},u32("source"),u32("target"),u32("connector"),item.get("sourcePath").string(),item.get("targetPath").string(),item.get("monitorPath").string(),item.get("complete").boolean()});
  }return scope;
}
inline std::string nativeRecoveryScopeBytes(const NativeRecoveryScope& scope){
  std::string bindings="[";for(const auto& item:scope.foreign){if(bindings.size()>1)bindings+=',';bindings+="{\"sourceAdapterHigh\":"+std::to_string(item.sourceAdapter.HighPart)+",\"sourceAdapterLow\":"+std::to_string(item.sourceAdapter.LowPart)+",\"source\":"+std::to_string(item.source)+",\"targetAdapterHigh\":"+std::to_string(item.targetAdapter.HighPart)+",\"targetAdapterLow\":"+std::to_string(item.targetAdapter.LowPart)+",\"target\":"+std::to_string(item.target)+",\"connector\":"+std::to_string(item.connector)+",\"sourcePath\":"+jsonQuote(item.sourcePath)+",\"targetPath\":"+jsonQuote(item.targetPath)+",\"monitorPath\":"+jsonQuote(item.monitorPath)+",\"complete\":"+(item.complete?"true":"false")+'}';}
  return "{\"session\":"+std::to_string(scope.session)+",\"logon\":"+jsonQuote(scope.logon)+",\"tick\":"+jsonQuote(std::to_string(scope.tick))+",\"foreign\":"+bindings+"]}";
}
inline void validateNativeRecoveryBaseline(const NativeRecoveryBaseline& value){
  if(!nativeDisplayInstanceId(value.instance)||!value.generation||value.topology.ccd.paths.empty()||value.topology.ccd.paths.size()>64||value.topology.ccd.modes.size()>256)throw std::runtime_error("native_baseline_invalid");
  const auto& baseline=value.topology;const auto captured=captureNativeTopologyBaseline(baseline.ccd,baseline.targets,baseline.replacedMonitor);
  if(captured.placements.size()!=baseline.placements.size())throw std::runtime_error("native_baseline_invalid");
  for(size_t i=0;i<captured.placements.size();++i){const auto& a=captured.placements[i];const auto& b=baseline.placements[i];if(a.id!=b.id||a.left!=b.left||a.top!=b.top||a.primary!=b.primary)throw std::runtime_error("native_baseline_invalid");}
  if(!nativeTopologyMatches(planNativeTopology(baseline.ccd,baseline.targets,baseline.placements),baseline.ccd,baseline.ccd))throw std::runtime_error("native_baseline_invalid");
  if(value.scope)validateNativeRecoveryScope(*value.scope,baseline);
}
inline NativeRecoveryBaseline parseNativeRecoveryBaseline(const std::string& bytes){
  if(bytes.empty()||bytes.size()>nativeBaselineByteLimit)throw std::runtime_error("native_baseline_invalid");const auto root=JsonParser::parse(bytes);const auto* fields=std::get_if<Json::Object>(&root.value);const auto* paths=std::get_if<Json::Array>(&root.get("paths").value);const auto generation=root.get("generation").number();
  const auto schema=root.get("schema").number();if(!fields||!((schema==1&&fields->size()==7)||(schema==2&&fields->size()==8))||root.get("provider").string()!="libvirtualdisplay-1.6.4"||!nativeDisplayInteger(generation,1,UINT32_MAX)||!paths||paths->empty()||paths->size()>64)throw std::runtime_error("native_baseline_invalid");
  NativeRecoveryBaseline result;result.instance=root.get("deviceInstanceId").string();result.generation=uint32_t(generation);result.topology.replacedMonitor=root.get("replacedMonitor").string();if(schema==2)result.scope=parseNativeRecoveryScope(root.get("scope"));auto targetEnvelope=root;std::get<Json::Object>(targetEnvelope.value)["leaseActive"].value=true;std::get<Json::Object>(targetEnvelope.value)["schema"].value=1.0;result.topology.targets=nativeDisplayTargets(targetEnvelope);
  for(const auto& item:std::get<Json::Array>(root.get("targets").value))if(!std::holds_alternative<Json::Object>(item.value)||std::get<Json::Object>(item.value).size()!=6)throw std::runtime_error("native_baseline_invalid");
  auto& ccd=result.topology.ccd;std::map<std::tuple<int,LONG,DWORD,UINT32>,UINT32> modes;std::set<std::tuple<LONG,DWORD,UINT32>> targetIds;
  const auto modeIndex=[&](const DISPLAYCONFIG_MODE_INFO& value){const auto key=std::tuple{int(value.infoType),value.adapterId.HighPart,value.adapterId.LowPart,value.id};const auto found=modes.find(key);if(found!=modes.end()){if(!nativeSameMode(ccd.modes[found->second],value))throw std::runtime_error("native_baseline_invalid");return found->second;}const auto index=UINT32(ccd.modes.size());ccd.modes.push_back(value);modes.emplace(key,index);return index;};
  for(const auto& encoded:*paths){const auto* row=std::get_if<Json::Array>(&encoded.value);if(!row||(row->size()!=34&&row->size()!=47))throw std::runtime_error("native_baseline_invalid");
    const auto integer=[&](size_t index,int64_t low,int64_t high){return nativeBaselineInteger((*row)[index],low,high);};const auto u32=[&](size_t index){return UINT32(integer(index,0,UINT32_MAX));};const auto i32=[&](size_t index){return LONG(integer(index,INT32_MIN,INT32_MAX));};
    DISPLAYCONFIG_PATH_INFO path{};path.sourceInfo.adapterId={u32(1),i32(0)};path.sourceInfo.id=u32(2);path.targetInfo.adapterId={u32(4),i32(3)};path.targetInfo.id=u32(5);path.flags=u32(6);path.targetInfo.targetAvailable=BOOL(integer(7,1,1));
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||!targetIds.emplace(path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id).second)throw std::runtime_error("native_baseline_invalid");
    path.targetInfo.outputTechnology=static_cast<DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY>(i32(8));path.targetInfo.rotation=static_cast<DISPLAYCONFIG_ROTATION>(integer(9,1,4));const auto scaling=integer(10,1,128);if(scaling>5&&scaling!=128)throw std::runtime_error("native_baseline_invalid");path.targetInfo.scaling=static_cast<DISPLAYCONFIG_SCALING>(scaling);path.targetInfo.refreshRate={u32(11),u32(12)};path.targetInfo.scanLineOrdering=static_cast<DISPLAYCONFIG_SCANLINE_ORDERING>(integer(13,0,3));
    DISPLAYCONFIG_MODE_INFO source{};source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId=path.sourceInfo.adapterId;source.id=path.sourceInfo.id;source.sourceMode={DWORD(integer(14,1,UINT32_MAX)),DWORD(integer(15,1,UINT32_MAX)),static_cast<DISPLAYCONFIG_PIXELFORMAT>(integer(16,1,5)),{i32(17),i32(18)}};
    if(int64_t(source.sourceMode.position.x)+source.sourceMode.width>INT32_MAX||int64_t(source.sourceMode.position.y)+source.sourceMode.height>INT32_MAX)throw std::runtime_error("native_baseline_invalid");
    DISPLAYCONFIG_MODE_INFO target{};target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId=path.targetInfo.adapterId;target.id=path.targetInfo.id;auto& signal=target.targetMode.targetVideoSignalInfo;signal.pixelRate=UINT64(integer(19,0,INT64_MAX));signal.hSyncFreq={u32(20),u32(21)};signal.vSyncFreq={u32(22),u32(23)};signal.activeSize={u32(24),u32(25)};signal.totalSize={u32(26),u32(27)};if(!signal.activeSize.cx||!signal.activeSize.cy||!signal.totalSize.cx||!signal.totalSize.cy)throw std::runtime_error("native_baseline_invalid");signal.videoStandard=u32(28);signal.scanLineOrdering=static_cast<DISPLAYCONFIG_SCANLINE_ORDERING>(integer(29,0,3));
    const bool desktop=integer(30,0,1)!=0,virtualMode=bool(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE);if(row->size()!=(desktop?47:34)||desktop&&!virtualMode)throw std::runtime_error("native_baseline_invalid");
    const auto sourceIndex=modeIndex(source),targetIndex=modeIndex(target);const auto tail=row->size()-3;path.sourceInfo.statusFlags=u32(tail);path.targetInfo.statusFlags=u32(tail+1);
    if(virtualMode){path.sourceInfo.cloneGroupId=UINT32(integer(tail+2,0,65535));path.sourceInfo.sourceModeInfoIdx=sourceIndex;path.targetInfo.targetModeInfoIdx=targetIndex;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
      if(desktop){DISPLAYCONFIG_MODE_INFO image{};image.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;image.adapterId={DWORD(i32(32)),i32(31)};image.id=DWORD(i32(33));auto& info=image.desktopImageInfo;info.PathSourceSize={LONG(integer(34,1,INT32_MAX)),LONG(integer(35,1,INT32_MAX))};info.DesktopImageRegion={i32(36),i32(37),i32(38),i32(39)};info.DesktopImageClip={i32(40),i32(41),i32(42),i32(43)};if(info.DesktopImageRegion.right<info.DesktopImageRegion.left||info.DesktopImageRegion.bottom<info.DesktopImageRegion.top||info.DesktopImageClip.right<info.DesktopImageClip.left||info.DesktopImageClip.bottom<info.DesktopImageClip.top)throw std::runtime_error("native_baseline_invalid");path.targetInfo.desktopModeInfoIdx=modeIndex(image);}
    }else{(void)integer(tail+2,-1,-1);path.sourceInfo.modeInfoIdx=sourceIndex;path.targetInfo.modeInfoIdx=targetIndex;}
    ccd.paths.push_back(path);
  }
  result.topology.placements=captureNativeTopologyBaseline(ccd,result.topology.targets,result.topology.replacedMonitor).placements;validateNativeRecoveryBaseline(result);return result;
}
inline std::string nativeRecoveryBaselineBytes(const NativeRecoveryBaseline& value){
  validateNativeRecoveryBaseline(value);std::string targets="[",paths="[";
  for(const auto& target:value.topology.targets){if(targets.size()>1)targets+=',';targets+="{\"id\":"+jsonQuote(target.id)+",\"adapterHigh\":"+std::to_string(target.adapter.HighPart)+",\"adapterLow\":"+std::to_string(target.adapter.LowPart)+",\"target\":"+std::to_string(target.target)+",\"width\":"+std::to_string(target.width)+",\"height\":"+std::to_string(target.height)+'}';}
  for(const auto& path:value.topology.ccd.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){if(paths.size()>1)paths+=',';paths+='[';const auto row=nativeBaselinePathRow(value.topology.ccd,path);for(size_t i=0;i<row.size();++i){if(i)paths+=',';paths+=jsonQuote(std::to_string(row[i]));}paths+=']';}
  const auto bytes="{\"schema\":"+std::string(value.scope?"2":"1")+",\"provider\":\"libvirtualdisplay-1.6.4\",\"deviceInstanceId\":"+jsonQuote(value.instance)+",\"generation\":"+std::to_string(value.generation)+",\"replacedMonitor\":"+jsonQuote(value.topology.replacedMonitor)+",\"targets\":"+targets+"],\"paths\":"+paths+"]"+(value.scope?",\"scope\":"+nativeRecoveryScopeBytes(*value.scope):"")+'}';(void)parseNativeRecoveryBaseline(bytes);return bytes;
}
}
