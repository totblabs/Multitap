#pragma once
// Positive identification of a retired Multitap legacy virtual-display adapter
// (the older driver install), for DisplayV2Access::retired. All of these must
// hold; a name, a count or the vendor hardware ID alone is never enough:
//  - the device is a started Display-class ROOT device whose instance ID is
//    ROOT\MULTITAPVIRTUALDISPLAY\NNNN (our installer's name, not the vendor's);
//  - that instance equals the DeviceInstance owner marker the installer wrote
//    to HKLM\SOFTWARE\Multitap\Display;
//  - its hardware ID is the vendor driver's Root\MttVDD.
// A different product's MttVDD install fails the first two. Anything unresolved
// is "not retired", so nothing is ever treated as ours by accident.
#include "display_v2.hpp"
#include "native_stream_console_identity.hpp"
namespace host {
inline bool legacyDisplayTextEqual(std::wstring_view a,std::wstring_view b){return a.size()==b.size()&&_wcsnicmp(a.data(),b.data(),a.size())==0;}
inline bool legacyDisplayInstance(std::wstring_view instance){
  constexpr std::wstring_view prefix=L"ROOT\\MULTITAPVIRTUALDISPLAY\\";
  if(instance.size()!=prefix.size()+4||_wcsnicmp(instance.data(),prefix.data(),prefix.size())!=0)return false;
  return std::all_of(instance.begin()+prefix.size(),instance.end(),[](wchar_t ch){return ch>=L'0'&&ch<=L'9';});
}
inline bool legacyDisplayDevice(const NativeConsoleAdapterIdentity& device,std::wstring_view ownerInstance){
  if(!device.present||!nativeConsoleGuidEqual(device.classGuid,nativeConsoleDisplayClass)||!legacyDisplayTextEqual(device.enumerator,L"ROOT"))return false;
  if(!legacyDisplayInstance(device.instanceId)||!legacyDisplayTextEqual(device.instanceId,ownerInstance))return false;
  return std::any_of(device.hardwareIds.begin(),device.hardwareIds.end(),[](const auto& id){return legacyDisplayTextEqual(id,L"ROOT\\MTTVDD");});
}
// `resolve` maps an adapter LUID to its present device (nativeConsoleAdapterIdentity
// in production); `ownerInstance` reads the installer's owner marker.
inline DisplayV2Retired legacyRetiredResolver(NativeConsoleIdentityResolver resolve,std::function<std::wstring()> ownerInstance){
  return [resolve=std::move(resolve),ownerInstance=std::move(ownerInstance)](LUID adapter){
    try{
      if(!resolve||!ownerInstance)return false;
      const auto device=resolve(adapter);return device&&legacyDisplayDevice(*device,ownerInstance());
    }catch(...){return false;}
  };
}
// The owner marker from the machine-wide (64-bit) registry; empty when absent.
inline std::wstring legacyOwnerInstanceFromRegistry(){
  wchar_t value[128]{};DWORD bytes=sizeof(value)-sizeof(wchar_t);
  if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Multitap\\Display",L"DeviceInstance",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,value,&bytes)!=ERROR_SUCCESS)return {};
  return value;
}
// A legacy device may only go when the Host no longer uses it: the native provider
// is explicitly configured and actually ready, and no saved display state is
// waiting to be restored (a restore may still name the legacy adapter).
inline bool legacyRetirementPermitted(bool providerConfirmed,bool nativeReady,bool statePresent){return providerConfirmed&&nativeReady&&!statePresent;}
// Removes the owner-marked legacy device (started, disabled or a registered
// phantom). Identity is re-proved from the live node right before the removal
// with the same rules as legacyDisplayDevice; anything else is refused. `removed`
// means the node was observed gone; `pending` means Windows still lists it (a
// restart may finish it). The driver package stays in the driver store.
enum class LegacyDisplayRetire:uint32_t {absent=0,removed=1,refused=2,failed=3,pending=4};
inline LegacyDisplayRetire retireLegacyDisplayDevice(std::wstring_view ownerInstance){
  if(ownerInstance.empty())return LegacyDisplayRetire::absent;
  if(!legacyDisplayInstance(ownerInstance))return LegacyDisplayRetire::refused;
  const std::wstring id(ownerInstance);
  struct Set {HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}} set{SetupDiCreateDeviceInfoList(&nativeConsoleDisplayClass,nullptr)};
  if(set.value==INVALID_HANDLE_VALUE)return LegacyDisplayRetire::failed;
  SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
  if(!SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&device))return GetLastError()==ERROR_NO_SUCH_DEVINST?LegacyDisplayRetire::absent:LegacyDisplayRetire::failed;
  const auto property=[&](DWORD key,DWORD wanted)->std::optional<std::vector<wchar_t>>{
    DWORD type=0,bytes=0;SetupDiGetDeviceRegistryPropertyW(set.value,&device,key,&type,nullptr,0,&bytes);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||type!=wanted||bytes<sizeof(wchar_t)||bytes>8192||bytes%sizeof(wchar_t))return {};
    std::vector<wchar_t> value(bytes/sizeof(wchar_t));DWORD actual=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set.value,&device,key,&type,reinterpret_cast<PBYTE>(value.data()),bytes,&actual)||type!=wanted||actual!=bytes||value.back()!=L'\0')return {};
    return value;
  };
  wchar_t live[201]{};DWORD length=0;
  if(!SetupDiGetDeviceInstanceIdW(set.value,&device,live,DWORD(std::size(live)),&length)||length<2||length>std::size(live))return LegacyDisplayRetire::refused;
  NativeConsoleAdapterIdentity identity;identity.instanceId.assign(live,length-1);identity.classGuid=device.ClassGuid;identity.present=true;
  const auto enumerator=property(SPDRP_ENUMERATOR_NAME,REG_SZ),hardware=property(SPDRP_HARDWAREID,REG_MULTI_SZ);
  if(!enumerator||!hardware||hardware->size()<2)return LegacyDisplayRetire::refused;
  identity.enumerator.assign(enumerator->data(),enumerator->size()-1);
  for(size_t at=0;at<hardware->size()&&(*hardware)[at];){
    const auto end=std::find(hardware->begin()+at,hardware->end(),L'\0');if(end==hardware->end()||identity.hardwareIds.size()>=16)return LegacyDisplayRetire::refused;
    identity.hardwareIds.emplace_back(hardware->data()+at,size_t(end-(hardware->begin()+at)));at=size_t(end-hardware->begin())+1;
  }
  if(!legacyDisplayDevice(identity,ownerInstance))return LegacyDisplayRetire::refused;
  if(!SetupDiCallClassInstaller(DIF_REMOVE,set.value,&device))return LegacyDisplayRetire::failed;
  DEVINST node=0;
  return CM_Locate_DevNodeW(&node,const_cast<DEVINSTID_W>(id.c_str()),CM_LOCATE_DEVNODE_PHANTOM)==CR_NO_SUCH_DEVNODE?LegacyDisplayRetire::removed:LegacyDisplayRetire::pending;
}
// After the device is gone: the installer's owner record goes, and the vendor path
// only when it is exactly our owned path (another product's value stays). The
// marker is deleted last, so a partial failure is simply tried again.
inline void clearLegacyDisplayOwnerRecord(){
  constexpr auto flags=RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY;
  wchar_t owned[1024]{},vendor[1024]{};DWORD ownedBytes=sizeof(owned)-sizeof(wchar_t),vendorBytes=sizeof(vendor)-sizeof(wchar_t);
  if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Multitap\\Display",L"PackagePath",flags,nullptr,owned,&ownedBytes)==ERROR_SUCCESS&&
     RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver",L"VDDPATH",flags,nullptr,vendor,&vendorBytes)==ERROR_SUCCESS&&_wcsicmp(owned,vendor)==0)
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver",L"VDDPATH");
  RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Multitap\\Display",L"PackagePath");
  RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Multitap\\Display",L"DeviceInstance");
}
} // namespace host
