#pragma once
#include "machine.hpp"
#include "native_display_contract.hpp"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <functional>
#include <optional>
#include <set>
#include <span>

namespace host {
// Private, candidate-only console identity. This is not authorization: callers
// must obtain the model proof from the authenticated broker and revalidate the
// current machine, lease and exact topology immediately before any mutation.
// Neither device identity nor interface paths belong in public diagnostics.
struct NativeConsoleModelProof { std::string machineUuid,model; };
struct NativeConsoleAdapterIdentity {
  LUID adapter{};
  std::wstring interfacePath,instanceId,enumerator;
  std::vector<std::wstring> hardwareIds;
  GUID classGuid{};
  bool present=false,started=false;
};
struct NativeConsolePathKey {
  LUID sourceAdapter{},targetAdapter{};UINT32 source=0,target=0;
};
struct NativeConsoleBinding {
  NativeConsoleModelProof proof;
  NativeConsoleAdapterIdentity device;
  NativeConsolePathKey path;
};
struct NativeConsoleIdentityResult {
  std::optional<NativeConsoleBinding> binding;
  std::string reason;
};
using NativeConsoleIdentityResolver=std::function<std::optional<NativeConsoleAdapterIdentity>(LUID)>;
inline constexpr GUID nativeConsoleDisplayClass={0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
// GUID_DEVINTERFACE_DISPLAY_ADAPTER (Ntddvdeo.h). A device setup class is
// different from the interface class used by CCD's adapterDevicePath.
inline constexpr GUID nativeConsoleDisplayInterfaceClass={0x5b45201d,0xf2f2,0x4f3b,{0x85,0xbb,0x30,0xff,0x1f,0x95,0x35,0x99}};
inline bool nativeConsoleLuidEqual(LUID a,LUID b){return a.LowPart==b.LowPart&&a.HighPart==b.HighPart;}
inline bool nativeConsoleGuidEqual(const GUID& a,const GUID& b){return std::memcmp(&a,&b,sizeof(GUID))==0;}
inline bool nativeConsoleHex(wchar_t ch){return (ch>=L'0'&&ch<=L'9')||(ch>=L'A'&&ch<=L'F');}
inline std::optional<std::wstring> nativeConsoleAscii(std::wstring_view value,size_t limit){
  if(value.empty()||value.size()>limit)return {};
  std::wstring out;out.reserve(value.size());
  for(const auto ch:value){if(ch<L'!'||ch>L'~')return {};out.push_back(ch>=L'a'&&ch<=L'z'?wchar_t(ch-L'a'+L'A'):ch);}
  return out;
}
inline std::optional<std::string> nativeConsoleUuid(std::string_view value){
  if(value.size()!=36)return {};std::string out(value);bool anyNonZero=false,anyNonF=false;
  for(size_t i=0;i<out.size();++i){
    if(i==8||i==13||i==18||i==23){if(out[i]!='-')return {};continue;}
    auto& ch=out[i];if(ch>='A'&&ch<='F')ch=char(ch-'A'+'a');
    if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return {};
    anyNonZero=anyNonZero||ch!='0';anyNonF=anyNonF||ch!='f';
  }
  if(!anyNonZero||!anyNonF)return {};return out;
}
inline bool nativeConsolePciHardware(std::wstring_view value){
  constexpr std::wstring_view prefix=L"PCI\\VEN_1234&DEV_1111";
  if(!value.starts_with(prefix))return false;
  std::set<std::wstring> seen;size_t at=prefix.size();
  while(at<value.size()){
    if(value[at++]!=L'&')return false;
    const auto end=value.find(L'&',at);const auto token=value.substr(at,end==std::wstring_view::npos?value.size()-at:end-at);
    const auto split=token.find(L'_');if(split==std::wstring_view::npos)return false;
    const std::wstring kind(token.substr(0,split));const auto digits=token.substr(split+1);
    if(!seen.insert(kind).second||!((kind==L"SUBSYS"&&digits.size()==8)||(kind==L"REV"&&digits.size()==2)||(kind==L"CC"&&(digits.size()==4||digits.size()==6)))||
       !std::all_of(digits.begin(),digits.end(),nativeConsoleHex))return false;
    at=end==std::wstring_view::npos?value.size():end;
  }
  return true;
}
inline bool nativeConsolePciInstance(std::wstring_view value){
  const auto split=value.find(L'\\',4);
  if(split==std::wstring_view::npos||!nativeConsolePciHardware(value.substr(0,split))||split+1>=value.size())return false;
  const auto suffix=value.substr(split+1);
  return std::all_of(suffix.begin(),suffix.end(),[](wchar_t ch){return (ch>=L'A'&&ch<=L'Z')||(ch>=L'0'&&ch<=L'9')||ch==L'&'||ch==L'_';});
}
inline std::optional<NativeConsoleAdapterIdentity> canonicalNativeConsoleAdapter(const NativeConsoleAdapterIdentity& raw){
  if((!raw.adapter.LowPart&&!raw.adapter.HighPart)||!raw.present||!raw.started||!nativeConsoleGuidEqual(raw.classGuid,nativeConsoleDisplayClass)||raw.hardwareIds.empty()||raw.hardwareIds.size()>16)return {};
  const auto path=nativeConsoleAscii(raw.interfacePath,127),instance=nativeConsoleAscii(raw.instanceId,200),enumerator=nativeConsoleAscii(raw.enumerator,32);
  if(!path||!instance||!enumerator||path->size()<=4||!path->starts_with(L"\\\\?\\")||path->find(L'/')!=std::wstring::npos)return {};
  NativeConsoleAdapterIdentity result=raw;result.interfacePath=*path;result.instanceId=*instance;result.enumerator=*enumerator;result.hardwareIds.clear();
  for(const auto& id:raw.hardwareIds){const auto canonical=nativeConsoleAscii(id,200);if(!canonical)return {};result.hardwareIds.push_back(*canonical);}
  std::sort(result.hardwareIds.begin(),result.hardwareIds.end());
  if(std::adjacent_find(result.hardwareIds.begin(),result.hardwareIds.end())!=result.hardwareIds.end())return {};
  return result;
}
inline bool nativeConsoleIsStdAdapter(const NativeConsoleAdapterIdentity& device){
  return device.enumerator==L"PCI"&&nativeConsolePciInstance(device.instanceId)&&
    std::all_of(device.hardwareIds.begin(),device.hardwareIds.end(),[](const auto& id){return nativeConsolePciHardware(id);});
}
inline std::optional<NativeConsoleBinding> canonicalNativeConsoleBinding(const NativeConsoleBinding& raw){
  const auto uuid=nativeConsoleUuid(raw.proof.machineUuid);const auto device=canonicalNativeConsoleAdapter(raw.device);
  if(!uuid||raw.proof.model!="std"||!device||!nativeConsoleIsStdAdapter(*device)||
     !nativeConsoleLuidEqual(raw.path.sourceAdapter,raw.path.targetAdapter)||!nativeConsoleLuidEqual(raw.path.targetAdapter,device->adapter)||
     raw.path.source==UINT32_MAX||raw.path.target==UINT32_MAX)return {};
  auto result=raw;result.proof.machineUuid=*uuid;result.device=*device;return result;
}
inline bool sameNativeConsoleBinding(const NativeConsoleBinding& a,const NativeConsoleBinding& b){
  const auto x=canonicalNativeConsoleBinding(a),y=canonicalNativeConsoleBinding(b);if(!x||!y)return false;
  return x->proof.machineUuid==y->proof.machineUuid&&x->proof.model==y->proof.model&&
    nativeConsoleLuidEqual(x->path.sourceAdapter,y->path.sourceAdapter)&&nativeConsoleLuidEqual(x->path.targetAdapter,y->path.targetAdapter)&&x->path.source==y->path.source&&x->path.target==y->path.target&&
    x->device.interfacePath==y->device.interfacePath&&x->device.instanceId==y->device.instanceId&&x->device.enumerator==y->device.enumerator&&x->device.hardwareIds==y->device.hardwareIds;
}
inline NativeConsoleIdentityResult identifyNativeStreamConsole(const NativeConsoleModelProof& proof,std::string_view currentMachineUuid,bool remoteSession,
    std::span<const DISPLAYCONFIG_PATH_INFO> paths,std::span<const NativeDisplayTarget> owned,const NativeConsoleIdentityResolver& resolver){
  const auto fail=[](const char* reason){return NativeConsoleIdentityResult{{},reason};};
  if(remoteSession)return fail("console_identity_remote_session");
  const auto expected=nativeConsoleUuid(proof.machineUuid),current=nativeConsoleUuid(currentMachineUuid);
  if(proof.model!="std"||!expected||!current||*expected!=*current)return fail("console_identity_model_unverified");
  if(!resolver||paths.empty()||paths.size()>4096||owned.empty()||owned.size()>2)return fail("console_identity_topology_invalid");
  size_t activeCount=0;std::optional<NativeConsoleBinding> selected;
  try{
    for(const auto& path:paths){
      if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE))continue;
      if(++activeCount>64||!path.targetInfo.targetAvailable)return fail("console_identity_topology_invalid");
      if(std::any_of(owned.begin(),owned.end(),[&](const auto& target){return nativeConsoleLuidEqual(target.adapter,path.targetInfo.adapterId)&&target.target==path.targetInfo.id;}))continue;
      const auto raw=resolver(path.targetInfo.adapterId);
      if(!raw||!nativeConsoleLuidEqual(raw->adapter,path.targetInfo.adapterId))return fail("console_identity_unresolved");
      const auto device=canonicalNativeConsoleAdapter(*raw);if(!device)return fail("console_identity_unresolved");
      if(!nativeConsoleIsStdAdapter(*device))continue;
      if(!nativeConsoleLuidEqual(path.sourceInfo.adapterId,path.targetInfo.adapterId))return fail("console_identity_cross_adapter");
      if(std::any_of(owned.begin(),owned.end(),[&](const auto& target){return nativeConsoleLuidEqual(target.adapter,device->adapter);}))return fail("console_identity_owned_adapter");
      if(selected)return fail("console_identity_ambiguous");
      selected=canonicalNativeConsoleBinding({{*expected,proof.model},*device,{path.sourceInfo.adapterId,path.targetInfo.adapterId,path.sourceInfo.id,path.targetInfo.id}});
      if(!selected)return fail("console_identity_unresolved");
    }
  }catch(...){return fail("console_identity_unresolved");}
  if(!selected)return fail("console_identity_not_found");
  // Clones, cross-adapter sources and duplicate active paths on this adapter are
  // deliberately not reduced to one candidate by choosing the first target.
  const auto touching=std::count_if(paths.begin(),paths.end(),[&](const auto& path){return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&
    (nativeConsoleLuidEqual(path.sourceInfo.adapterId,selected->device.adapter)||nativeConsoleLuidEqual(path.targetInfo.adapterId,selected->device.adapter));});
  if(touching!=1)return fail("console_identity_ambiguous");
  return {std::move(selected),"console_identity_verified"};
}

// Read-only candidate evidence for a console-output preparation operation.
// Inactive CCD alternatives can name several sources for one target: report
// only that target, never choose a source or grant permission to activate it.
// The caller still needs its current authenticated operation/lease binding.
struct NativeConsoleOutputKey {LUID adapter{};UINT32 target=UINT32_MAX;};
struct NativeQemuConsoleOutput {
  NativeConsoleModelProof proof;NativeConsoleAdapterIdentity device;
  UINT32 target=UINT32_MAX;bool active=false;
};
struct NativeQemuConsoleObservation {std::optional<NativeQemuConsoleOutput> output;std::string reason;};
inline NativeQemuConsoleObservation observeNativeQemuConsoleOutput(const NativeConsoleModelProof& proof,std::string_view currentMachineUuid,bool remoteSession,
    std::span<const DISPLAYCONFIG_PATH_INFO> paths,std::span<const NativeDisplayTarget> owned,const NativeConsoleIdentityResolver& resolver,
    const std::optional<NativeConsoleOutputKey>& expected={}){
  const auto fail=[](const char* reason){return NativeQemuConsoleObservation{{},reason};};
  const auto machine=nativeConsoleUuid(proof.machineUuid),current=nativeConsoleUuid(currentMachineUuid);
  if(remoteSession)return fail("console_identity_remote_session");
  if(proof.model!="std"||!machine||!current||*machine!=*current)return fail("console_identity_model_unverified");
  if(!resolver||paths.empty()||paths.size()>4096||owned.size()>2)return fail("console_identity_topology_invalid");
  const auto key=[](LUID value){return (uint64_t(uint32_t(value.HighPart))<<32)|value.LowPart;};
  std::set<std::string> names;std::set<std::pair<uint64_t,UINT32>> ownedTargets;
  for(const auto& item:owned)if((item.id!="0"&&item.id!="1")||item.target==UINT32_MAX||!names.insert(item.id).second||!ownedTargets.insert({key(item.adapter),item.target}).second)return fail("console_identity_topology_invalid");
  if(expected&&expected->target==UINT32_MAX)return fail("console_identity_topology_invalid");
  std::map<uint64_t,NativeConsoleAdapterIdentity> devices;std::optional<NativeQemuConsoleOutput> selected;size_t activePaths=0,selectedActivePaths=0;
  try{
    for(const auto& path:paths){
      if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&++activePaths>64)return fail("console_identity_topology_invalid");
      if(!path.targetInfo.targetAvailable)continue;
      if(ownedTargets.count({key(path.targetInfo.adapterId),path.targetInfo.id}))continue;
      const auto adapter=key(path.targetInfo.adapterId);auto device=devices.find(adapter);
      if(device==devices.end()){
        if(devices.size()>=64)return fail("console_identity_topology_invalid");
        const auto raw=resolver(path.targetInfo.adapterId);
        if(!raw||!nativeConsoleLuidEqual(raw->adapter,path.targetInfo.adapterId))return fail("console_identity_unresolved");
        const auto canonical=canonicalNativeConsoleAdapter(*raw);if(!canonical)return fail("console_identity_unresolved");
        device=devices.emplace(adapter,*canonical).first;
      }
      if(!nativeConsoleIsStdAdapter(device->second))continue;
      if(std::any_of(owned.begin(),owned.end(),[&](const auto& item){return nativeConsoleLuidEqual(item.adapter,path.targetInfo.adapterId);}))return fail("console_identity_owned_adapter");
      if(!nativeConsoleLuidEqual(path.sourceInfo.adapterId,path.targetInfo.adapterId))return fail("console_identity_cross_adapter");
      if(path.targetInfo.id==UINT32_MAX||path.sourceInfo.id==UINT32_MAX)return fail("console_identity_topology_invalid");
      if(selected&&(!nativeConsoleLuidEqual(selected->device.adapter,path.targetInfo.adapterId)||selected->target!=path.targetInfo.id))return fail("console_identity_ambiguous");
      if(!selected)selected=NativeQemuConsoleOutput{{*machine,"std"},device->second,path.targetInfo.id,false};
      if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){if(++selectedActivePaths>1)return fail("console_identity_ambiguous");selected->active=true;}
    }
    if(!selected)return fail("console_identity_not_found");
    for(const auto& path:paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeConsoleLuidEqual(path.sourceInfo.adapterId,selected->device.adapter)&&
        (!nativeConsoleLuidEqual(path.targetInfo.adapterId,selected->device.adapter)||path.targetInfo.id!=selected->target))return fail("console_identity_cross_adapter");
    if(expected&&(!nativeConsoleLuidEqual(expected->adapter,selected->device.adapter)||expected->target!=selected->target))return fail("console_identity_binding_changed");
  }catch(...){return fail("console_identity_unresolved");}
  const auto reason=selected->active?"console_output_active":"console_output_inactive";
  return {std::move(selected),reason};
}

// Live resolver is read-only and intentionally not called by the pure test.
// CCD's opaque adapter interface is resolved through SetupAPI, never by a
// friendly-name or substring match. Enumerate present adapter interfaces before
// DIODI_NO_ADD; a Display setup-class set does not include those interfaces.
// The resolved devnode must still belong to the Display setup class below.
inline std::optional<NativeConsoleAdapterIdentity> nativeConsoleAdapterIdentity(LUID adapter){
  DISPLAYCONFIG_ADAPTER_NAME name{};name.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;name.header.size=sizeof(name);name.header.adapterId=adapter;
  if(DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS)return {};
  const auto nameEnd=std::find(std::begin(name.adapterDevicePath),std::end(name.adapterDevicePath),L'\0');
  if(nameEnd==std::end(name.adapterDevicePath))return {};
  const auto canonicalPath=nativeConsoleAscii(std::wstring_view(name.adapterDevicePath,size_t(nameEnd-name.adapterDevicePath)),127);if(!canonicalPath)return {};
  struct DeviceSet {HDEVINFO value=INVALID_HANDLE_VALUE;~DeviceSet(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}};
  DeviceSet set{SetupDiGetClassDevsW(&nativeConsoleDisplayInterfaceClass,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};if(set.value==INVALID_HANDLE_VALUE)return {};
  SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);
  if(!SetupDiOpenDeviceInterfaceW(set.value,name.adapterDevicePath,DIODI_NO_ADD,&iface)||!(iface.Flags&SPINT_ACTIVE)||!nativeConsoleGuidEqual(iface.InterfaceClassGuid,nativeConsoleDisplayInterfaceClass))return {};
  DWORD required=0;SetupDiGetDeviceInterfaceDetailW(set.value,&iface,nullptr,0,&required,nullptr);
  if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||required<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)||required>4096)return {};
  std::vector<BYTE> buffer(required);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());detail->cbSize=sizeof(*detail);
  SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
  if(!SetupDiGetDeviceInterfaceDetailW(set.value,&iface,detail,required,nullptr,&device)||!nativeConsoleGuidEqual(device.ClassGuid,nativeConsoleDisplayClass))return {};
  const auto capacity=(buffer.size()-offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W,DevicePath))/sizeof(wchar_t);
  const auto pathEnd=std::find(detail->DevicePath,detail->DevicePath+capacity,L'\0');if(pathEnd==detail->DevicePath+capacity)return {};
  const auto detailPath=nativeConsoleAscii(std::wstring_view(detail->DevicePath,size_t(pathEnd-detail->DevicePath)),127);if(!detailPath||*detailPath!=*canonicalPath)return {};
  wchar_t instance[201]{};DWORD instanceLength=0;
  if(!SetupDiGetDeviceInstanceIdW(set.value,&device,instance,DWORD(std::size(instance)),&instanceLength)||instanceLength<2||instanceLength>std::size(instance)||instance[instanceLength-1]!=L'\0')return {};
  NativeConsoleAdapterIdentity result;result.adapter=adapter;result.interfacePath=*canonicalPath;result.instanceId.assign(instance,instanceLength-1);result.classGuid=device.ClassGuid;
  const auto property=[&](DWORD key,DWORD wanted)->std::optional<std::vector<wchar_t>>{
    DWORD type=0,bytes=0;SetupDiGetDeviceRegistryPropertyW(set.value,&device,key,&type,nullptr,0,&bytes);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||type!=wanted||bytes<sizeof(wchar_t)||bytes>8192||bytes%sizeof(wchar_t))return {};
    std::vector<wchar_t> value(bytes/sizeof(wchar_t));DWORD actual=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set.value,&device,key,&type,reinterpret_cast<PBYTE>(value.data()),bytes,&actual)||type!=wanted||actual!=bytes||value.back()!=L'\0')return {};
    return value;
  };
  const auto enumeration=property(SPDRP_ENUMERATOR_NAME,REG_SZ),hardware=property(SPDRP_HARDWAREID,REG_MULTI_SZ);if(!enumeration||!hardware||hardware->size()<2)return {};
  if(std::find(enumeration->begin(),enumeration->end()-1,L'\0')!=enumeration->end()-1)return {};
  result.enumerator.assign(enumeration->data(),enumeration->size()-1);
  size_t at=0;
  while(at<hardware->size()&&(*hardware)[at]){
    const auto end=std::find(hardware->begin()+at,hardware->end(),L'\0');if(end==hardware->end()||result.hardwareIds.size()>=16)return {};
    result.hardwareIds.emplace_back(hardware->data()+at,size_t(end-(hardware->begin()+at)));at=size_t(end-hardware->begin())+1;
  }
  if(at!=hardware->size()-1||hardware->back()!=L'\0')return {};
  ULONG status=0,problem=0;if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS)return {};
  result.present=true;result.started=(status&DN_STARTED)&&problem==0;
  return canonicalNativeConsoleAdapter(result);
}
} // namespace host
