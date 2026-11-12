#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
#include "json_output.hpp"
#include "../../native/windows_video/frame_layout.hpp"

namespace fs = std::filesystem;
static constexpr wchar_t kOwner[] = L"SOFTWARE\\Multitap\\Display";
static constexpr wchar_t kVendor[] = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
static constexpr wchar_t kHardware[] = L"Root\\MttVDD";
static constexpr wchar_t kMarker[] = L"DeviceInstance";
struct Devices { HDEVINFO h; ~Devices() { if(h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h); } };
struct Reg { HKEY h{}; ~Reg() { if(h) RegCloseKey(h); } };
static void require(bool ok, const char* text) { if(!ok) throw std::runtime_error(text); }
static std::string utf8(const std::wstring& s) {
  if(s.empty()) return {};
  const int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
  std::string r(size_t(n),'\0'); WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr); return r;
}
static std::string json(const std::wstring& s) {
  return display_json::quote(s);
}
static std::wstring regRead(const wchar_t* path,const wchar_t* name) {
  wchar_t b[4096]{}; DWORD bytes=sizeof(b); return RegGetValueW(HKEY_LOCAL_MACHINE,path,name,RRF_RT_REG_SZ,nullptr,b,&bytes)==ERROR_SUCCESS?b:L"";
}
struct RegistryValue { std::wstring value; LSTATUS error{}; };
static RegistryValue registryValue(const wchar_t* path,const wchar_t* name) {
  wchar_t bytes[4096]{};DWORD count=sizeof(bytes);RegistryValue result;
  result.error=RegGetValueW(HKEY_LOCAL_MACHINE,path,name,RRF_RT_REG_SZ,nullptr,bytes,&count);
  if(result.error==ERROR_SUCCESS)result.value=bytes;
  return result;
}
static bool samePath(const std::wstring& a,const std::wstring& b) {
  if(a.empty()||b.empty())return false;
  auto left=fs::path(a).lexically_normal().wstring(),right=fs::path(b).lexically_normal().wstring();
  while(left.size()>3&&(left.back()==L'\\'||left.back()==L'/'))left.pop_back();
  while(right.size()>3&&(right.back()==L'\\'||right.back()==L'/'))right.pop_back();
  return !_wcsicmp(left.c_str(),right.c_str());
}
static void regWrite(const wchar_t* path,const wchar_t* name,const std::wstring& value) {
  Reg key; require(RegCreateKeyExW(HKEY_LOCAL_MACHINE,path,0,nullptr,0,KEY_SET_VALUE,nullptr,&key.h,nullptr)==ERROR_SUCCESS,"registry_write_denied");
  require(RegSetValueExW(key.h,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"registry_write_failed");
}
static bool elevated() { SID_IDENTIFIER_AUTHORITY nt=SECURITY_NT_AUTHORITY; PSID group=nullptr; BOOL member=FALSE; if(AllocateAndInitializeSid(&nt,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&group)){CheckTokenMembership(nullptr,group,&member);FreeSid(group);}return member!=FALSE; }
static std::wstring instance(HDEVINFO set,SP_DEVINFO_DATA& d) { wchar_t b[4096]{}; require(SetupDiGetDeviceInstanceIdW(set,&d,b,DWORD(std::size(b)),nullptr)!=FALSE,"device_identity_unavailable"); return b; }
static bool virtualDevice(HDEVINFO set,SP_DEVINFO_DATA& d) {
  wchar_t ids[4096]{}; if(!SetupDiGetDeviceRegistryPropertyW(set,&d,SPDRP_HARDWAREID,nullptr,reinterpret_cast<PBYTE>(ids),sizeof(ids),nullptr)) return false;
  for(const wchar_t* p=ids;p<ids+std::size(ids)&&*p;) {
    const size_t length=wcsnlen_s(p,size_t(ids+std::size(ids)-p));
    if(length==size_t(ids+std::size(ids)-p)) return false;
    if(!_wcsicmp(p,kHardware)||!_wcsicmp(p,L"MttVDD")) return true;
    p+=length+1;
  }
  return false;
}
struct Mode { DWORD width{},height{},refreshRate{}; };
struct Display { std::wstring name; int left{},top{},width{},height{}; bool primary{}; DWORD frequency{}; };
static std::vector<Mode> supportedModes(const std::wstring& name) {
  std::vector<Mode> result;
  for(DWORD i=0;i<4096&&result.size()<64;++i) {
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsExW(name.c_str(),i,&mode,0)) break;
    multitap::video::FrameLayout layout;
    if(mode.dmBitsPerPel!=32||mode.dmPelsWidth<320||mode.dmPelsHeight<180||!multitap::video::frameLayout(mode.dmPelsWidth,mode.dmPelsHeight,layout)||mode.dmDisplayFrequency<1||mode.dmDisplayFrequency>240)continue;
    if(std::none_of(result.begin(),result.end(),[&](const Mode& m){return m.width==mode.dmPelsWidth&&m.height==mode.dmPelsHeight&&m.refreshRate==mode.dmDisplayFrequency;})) result.push_back({mode.dmPelsWidth,mode.dmPelsHeight,mode.dmDisplayFrequency});
  }
  return result;
}
struct Adapter { bool owned{},matchingHardware{},statusAvailable{},started{},restartRequired{}; ULONG flags{},problem{}; CONFIGRET statusResult{}; std::wstring friendlyName; };
struct Status {
  bool installed{},ready{},owned{},restartRequired{},restartStatusAvailable{},ownerMarkerPresent{},vendorPathPresent{},vendorPathMatchesOwner{},vendorPathConflict{},ownerMarkerMissingDevice{},diagnosticsTruncated{};
  size_t deviceCount{},foreignDeviceCount{},sourceCount{},otherAdapterCount{};
  LONG enumerationError{},deviceEnumerationError{};LSTATUS ownerRegistryError{},vendorRegistryError{};
  const char* vendorPathState="unavailable";std::wstring id;std::vector<Display> displays;std::vector<Adapter> adapters;
};
static bool viewportRecoveryPending(const Status&s){
  if(!s.owned||s.foreignDeviceCount||s.vendorPathConflict)return false;
  const auto path=regRead(kOwner,L"PackagePath");
  return !path.empty()&&GetFileAttributesW((fs::path(path)/L"multitap-viewport-pending.txt").c_str())!=INVALID_FILE_ATTRIBUTES;
}
static bool consoleDesktop() {
  DWORD session=0;
  return ProcessIdToSessionId(GetCurrentProcessId(),&session)&&session!=0&&session==WTSGetActiveConsoleSessionId()&&!GetSystemMetrics(SM_REMOTESESSION);
}
static bool unlockedDefaultDesktop() {
  const auto input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
  if(!input)return false;
  wchar_t name[64]{};DWORD needed=0;
  const bool result=GetUserObjectInformationW(input,UOI_NAME,name,sizeof(name),&needed)&&!_wcsicmp(name,L"Default");
  CloseDesktop(input);return result;
}
static std::wstring deviceText(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property) {
  wchar_t value[512]{};DWORD type=0;
  if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,reinterpret_cast<PBYTE>(value),sizeof(value),nullptr)||type!=REG_SZ)return {};
  value[std::size(value)-1]=0;return value;
}
static const char* statusReason(const Status& s,bool console) {
  if(s.deviceEnumerationError)return "device_enumeration_failed";
  if(s.ownerRegistryError!=ERROR_SUCCESS&&s.ownerRegistryError!=ERROR_FILE_NOT_FOUND&&s.ownerRegistryError!=ERROR_PATH_NOT_FOUND)return "ownership_registry_unavailable";
  if(s.ownerMarkerMissingDevice)return "owned_virtual_display_device_missing";
  if(!s.installed)return "virtual_display_not_installed";
  if(!s.owned)return "virtual_display_not_owned";
  if(s.foreignDeviceCount)return "existing_virtual_display_requires_review";
  if(s.vendorPathConflict)return "shared_vendor_path_conflict";
  if(s.restartRequired)return "reboot_required";
  if(viewportRecoveryPending(s))return "viewport_recovery_required";
  if(!s.ready)return "virtual_display_device_not_ready";
  if(s.enumerationError)return "display_topology_unavailable";
  if(s.displays.empty())return console?"virtual_display_not_active_in_session":"interactive_console_required";
  if(s.displays.size()>2)return "virtual_display_count_not_supported";
  return "";
}
static std::wstring adapterInstance(const wchar_t* path) {
  Devices set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
  if(set.h==INVALID_HANDLE_VALUE) return {};
  SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
  if(!SetupDiOpenDeviceInterfaceW(set.h,path,0,&iface)) return {};
  SP_DEVINFO_DATA device{sizeof(device)};
  // The interface path is opaque. SetupAPI resolves its actual PnP instance;
  // DISPLAY_DEVICE.DeviceID is a hardware id and must not be compared to it.
  SetupDiGetDeviceInterfaceDetailW(set.h,&iface,nullptr,0,nullptr,&device);
  if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER) return {};
  return instance(set.h,device);
}
static std::vector<std::wstring> displayNames(const std::wstring& deviceId,LONG& error) {
  std::vector<std::wstring> result;
  for(unsigned attempt=0;attempt<3;++attempt) {
    UINT32 paths=0,modes=0;
    error=GetDisplayConfigBufferSizes(QDC_ALL_PATHS,&paths,&modes);
    if(error!=ERROR_SUCCESS) return result;
    if(paths>4096||modes>8192) {error=ERROR_INSUFFICIENT_BUFFER;return result;}
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths); std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    error=QueryDisplayConfig(QDC_ALL_PATHS,&paths,p.data(),&modes,m.data(),nullptr);
    if(error==ERROR_INSUFFICIENT_BUFFER) continue;
    if(error!=ERROR_SUCCESS) return result;
    for(UINT32 i=0;i<paths;++i) {
      if(!p[i].targetInfo.targetAvailable) continue;
      DISPLAYCONFIG_ADAPTER_NAME adapter{};adapter.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME,sizeof(adapter),p[i].sourceInfo.adapterId,0};
      if(DisplayConfigGetDeviceInfo(&adapter.header)!=ERROR_SUCCESS) continue;
      const auto actual=adapterInstance(adapter.adapterDevicePath);
      if(actual.empty()||_wcsicmp(actual.c_str(),deviceId.c_str())) continue;
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),p[i].sourceInfo.adapterId,p[i].sourceInfo.id};
      if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS) continue;
      const std::wstring name=source.viewGdiDeviceName;
      if(name.rfind(L"\\\\.\\DISPLAY",0)==0 && std::find(result.begin(),result.end(),name)==result.end()) result.push_back(name);
    }
    std::sort(result.begin(),result.end()); return result;
  }
  return result;
}
static std::vector<Display> activeDisplays() {
  std::vector<Display> result;
  for(DWORD n=0;;++n) {
    DISPLAY_DEVICEW dd{sizeof(dd)};if(!EnumDisplayDevicesW(nullptr,n,&dd,0)) break;
    if(!(dd.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)||(dd.StateFlags&DISPLAY_DEVICE_MIRRORING_DRIVER)) continue;
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsExW(dd.DeviceName,ENUM_CURRENT_SETTINGS,&mode,0)) continue;
    result.push_back({dd.DeviceName,mode.dmPosition.x,mode.dmPosition.y,int(mode.dmPelsWidth),int(mode.dmPelsHeight),(dd.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE)!=0,mode.dmDisplayFrequency});
  }
  return result;
}
static Status status() {
  Status s;const auto owner=registryValue(kOwner,kMarker),vendor=registryValue(kVendor,L"VDDPATH"),ownedPath=registryValue(kOwner,L"PackagePath");
  s.ownerMarkerPresent=!owner.value.empty();s.ownerRegistryError=owner.error;s.vendorRegistryError=vendor.error;s.vendorPathPresent=!vendor.value.empty();
  s.vendorPathMatchesOwner=samePath(vendor.value,ownedPath.value);
  if(vendor.error==ERROR_FILE_NOT_FOUND||vendor.error==ERROR_PATH_NOT_FOUND||(vendor.error==ERROR_SUCCESS&&!s.vendorPathPresent))s.vendorPathState="unset";
  else if(vendor.error==ERROR_SUCCESS&&s.vendorPathPresent) {
    if(owner.error==ERROR_FILE_NOT_FOUND||owner.error==ERROR_PATH_NOT_FOUND||(owner.error==ERROR_SUCCESS&&!s.ownerMarkerPresent)){s.vendorPathState="unowned";s.vendorPathConflict=true;}
    else if(owner.error==ERROR_SUCCESS&&s.ownerMarkerPresent&&ownedPath.error==ERROR_SUCCESS&&!ownedPath.value.empty()){s.vendorPathState=s.vendorPathMatchesOwner?"owned":"different";s.vendorPathConflict=!s.vendorPathMatchesOwner;}
    else s.vendorPathState="ownership_unknown";
  }
  Devices set{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,nullptr,nullptr,0)};
  if(set.h==INVALID_HANDLE_VALUE){s.deviceEnumerationError=LONG(GetLastError());return s;}
  SP_DEVINFO_DATA d{sizeof(d)};
  for(DWORD i=0;;++i) {
    if(!SetupDiEnumDeviceInfo(set.h,i,&d)){const auto error=GetLastError();if(error!=ERROR_NO_MORE_ITEMS)s.deviceEnumerationError=LONG(error);break;}
    const bool matching=virtualDevice(set.h,d);Adapter adapter;adapter.matchingHardware=matching;
    const auto id=instance(set.h,d);adapter.owned=matching&&!owner.value.empty()&&!_wcsicmp(id.c_str(),owner.value.c_str());
    adapter.statusResult=CM_Get_DevNode_Status(&adapter.flags,&adapter.problem,d.DevInst,0);adapter.statusAvailable=adapter.statusResult==CR_SUCCESS;
    if(!(adapter.flags&DN_HAS_PROBLEM))adapter.problem=0;
    adapter.started=adapter.statusAvailable&&(adapter.flags&DN_STARTED)!=0&&!adapter.problem;
    adapter.restartRequired=adapter.statusAvailable&&((adapter.flags&DN_NEED_RESTART)!=0||adapter.problem==CM_PROB_NEED_RESTART);
    adapter.friendlyName=deviceText(set.h,d,SPDRP_FRIENDLYNAME);if(adapter.friendlyName.empty())adapter.friendlyName=deviceText(set.h,d,SPDRP_DEVICEDESC);
    if(s.adapters.size()<64)s.adapters.push_back(adapter);else s.diagnosticsTruncated=true;
    if(!matching){++s.otherAdapterCount;continue;}
    s.installed=true;++s.deviceCount;if(!adapter.owned)++s.foreignDeviceCount;
    if(!s.id.empty()&&!adapter.owned)continue;
    s.owned=adapter.owned;s.id=id;s.ready=adapter.started;
    if(adapter.owned){s.restartRequired=adapter.restartRequired;s.restartStatusAvailable=adapter.statusAvailable;}
  }
  s.ownerMarkerMissingDevice=s.ownerMarkerPresent&&!s.owned;
  if(!s.id.empty()) {
    const auto names=displayNames(s.id,s.enumerationError);
    s.sourceCount=names.size();
    for(const auto& display:activeDisplays()) if(std::find(names.begin(),names.end(),display.name)!=names.end()) s.displays.push_back(display);
  }
  std::sort(s.displays.begin(),s.displays.end(),[](const Display&a,const Display&b){return a.left!=b.left?a.left<b.left:a.top!=b.top?a.top<b.top:a.name<b.name;});
  return s;
}
static std::wstring logicalMonitorId(const Status&,const Display&,size_t);
static std::string statusJson(const Status& s) {
  std::ostringstream out; const Display d=s.displays.empty()?Display{}:s.displays.front();
  DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
  out<<std::boolalpha<<"{\"helperVersion\":\"0.1.9\",\"sessionId\":"<<session<<",\"remoteSession\":"<<(GetSystemMetrics(SM_REMOTESESSION)!=0)<<",\"installed\":"<<s.installed<<",\"deviceReady\":"<<s.ready<<",\"owned\":"<<s.owned<<",\"deviceCount\":"<<s.deviceCount<<",\"foreignDeviceCount\":"<<s.foreignDeviceCount<<",\"enumerationError\":"<<s.enumerationError<<",\"activeDisplay\":"<<!s.displays.empty()<<",\"displayDeviceName\":"<<json(d.name)<<",\"left\":"<<d.left<<",\"top\":"<<d.top<<",\"width\":"<<d.width<<",\"height\":"<<d.height<<",\"displayCount\":"<<s.displays.size()<<",\"displays\":[";
  for(size_t i=0;i<s.displays.size();++i) {
    const auto& a=s.displays[i];if(i)out<<',';
    out<<"{\"id\":"<<json(logicalMonitorId(s,a,i))<<",\"displayDeviceName\":"<<json(a.name)<<",\"left\":"<<a.left<<",\"top\":"<<a.top<<",\"width\":"<<a.width<<",\"height\":"<<a.height<<",\"refreshRate\":"<<a.frequency<<",\"primary\":"<<a.primary<<",\"virtual\":true,\"modes\":[";
    const auto modes=supportedModes(a.name);for(size_t n=0;n<modes.size();++n){if(n)out<<',';out<<"{\"width\":"<<modes[n].width<<",\"height\":"<<modes[n].height<<",\"refreshRate\":"<<modes[n].refreshRate<<'}';}out<<"]}";
  }
  const char* reason=statusReason(s,consoleDesktop());
  out<<"],\"configurationPending\":"<<(s.owned&&*reason)<<",\"recoveryRequired\":"<<viewportRecoveryPending(s)<<",\"rebootRequired\":"<<s.restartRequired<<",\"rebootStatusSource\":\""<<(s.restartStatusAvailable?"device_status":"unavailable")<<"\",\"reason\":\""<<reason<<"\",\"diagnostics\":{\"consoleSession\":"<<consoleDesktop()<<",\"activeConsoleSessionId\":"<<WTSGetActiveConsoleSessionId()<<",\"ownerMarkerPresent\":"<<s.ownerMarkerPresent<<",\"ownerMarkerMissingDevice\":"<<s.ownerMarkerMissingDevice<<",\"ownerRegistryError\":"<<s.ownerRegistryError<<",\"vendorRegistryError\":"<<s.vendorRegistryError<<",\"vendorPathState\":\""<<s.vendorPathState<<"\",\"vendorPathPresent\":"<<s.vendorPathPresent<<",\"vendorPathMatchesOwner\":"<<s.vendorPathMatchesOwner<<",\"vendorPathConflict\":"<<s.vendorPathConflict<<",\"deviceEnumerationError\":"<<s.deviceEnumerationError<<",\"availableSourceCount\":"<<s.sourceCount<<",\"otherAdapterCount\":"<<s.otherAdapterCount<<",\"truncated\":"<<s.diagnosticsTruncated<<",\"adapters\":[";
  for(size_t i=0;i<s.adapters.size();++i){if(i)out<<',';const auto& a=s.adapters[i];out<<"{\"friendlyName\":"<<json(a.friendlyName)<<",\"owned\":"<<a.owned<<",\"matchingHardware\":"<<a.matchingHardware<<",\"statusAvailable\":"<<a.statusAvailable<<",\"statusQueryResult\":"<<a.statusResult<<",\"started\":"<<a.started<<",\"statusFlags\":"<<a.flags<<",\"problemCode\":"<<a.problem<<",\"restartRequired\":"<<a.restartRequired<<'}';}
  out<<"]},\"error\":\""<<reason<<"\"}";return out.str();
}
template<typename Parent> static bool ownedAncestor(DEVINST node,DEVINST owner,Parent parent) {
  std::array<DEVINST,16> visited{};size_t count=0;
  while(count<visited.size()) {
    DEVINST next{};if(!parent(node,next)||next==node||std::find(visited.begin(),visited.begin()+count,next)!=visited.begin()+count)return false;
    if(next==owner)return true;
    visited[count++]=node;node=next;
  }
  return false;
}
static bool ownedAncestor(DEVINST node,DEVINST owner) {
  return ownedAncestor(node,owner,[](DEVINST value,DEVINST& next){return CM_Get_Parent(&next,value,0)==CR_SUCCESS;});
}
struct Naming { bool adapterNamed{};size_t monitorNamesApplied{},ownedMonitorCount{};DWORD error{};const char* reason=""; };
static void setFriendlyName(HDEVINFO set,SP_DEVINFO_DATA& device,const std::wstring& name) {
  require(SetupDiSetDeviceRegistryPropertyW(set,&device,SPDRP_FRIENDLYNAME,reinterpret_cast<const BYTE*>(name.c_str()),DWORD((name.size()+1)*sizeof(wchar_t)))!=FALSE,"owned_device_naming_failed");
}
static Naming applyNames() {
  require(elevated(),"administrator_required");
  const auto owner=regRead(kOwner,kMarker);require(!owner.empty(),"no_owned_virtual_display");
  Devices adapters{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY,nullptr)};require(adapters.h!=INVALID_HANDLE_VALUE,"device_list_create_failed");
  SP_DEVINFO_DATA adapter{sizeof(adapter)};
  require(SetupDiOpenDeviceInfoW(adapters.h,owner.c_str(),nullptr,0,&adapter)&&virtualDevice(adapters.h,adapter)&&!_wcsicmp(instance(adapters.h,adapter).c_str(),owner.c_str()),"owned_virtual_display_identity_mismatch");
  Naming result;
  setFriendlyName(adapters.h,adapter,L"Multitap Virtual Display Adapter");result.adapterNamed=true;
  Devices monitors{SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR,nullptr,nullptr,DIGCF_PRESENT)};
  if(monitors.h==INVALID_HANDLE_VALUE){result.error=GetLastError();result.reason="monitor_enumeration_failed";return result;}
  struct Monitor { std::wstring id;SP_DEVINFO_DATA device; };
  std::vector<Monitor> owned;
  for(DWORD i=0;;++i) {
    SP_DEVINFO_DATA monitor{sizeof(monitor)};
    if(!SetupDiEnumDeviceInfo(monitors.h,i,&monitor)){if(GetLastError()!=ERROR_NO_MORE_ITEMS){result.error=GetLastError();result.reason="monitor_enumeration_failed";}break;}
    // Friendly strings, hardware IDs, and another virtual product's display
    // topology never establish monitor ownership. Only the PnP parent chain
    // back to our exact recorded adapter does.
    if(ownedAncestor(monitor.DevInst,adapter.DevInst))owned.push_back({instance(monitors.h,monitor),monitor});
  }
  result.ownedMonitorCount=owned.size();
  if(result.error)return result;
  if(owned.size()!=2){result.reason="owned_monitor_count_not_two";return result;}
  std::sort(owned.begin(),owned.end(),[](const Monitor& a,const Monitor& b){return a.id<b.id;});
  for(size_t i=0;i<owned.size();++i) {
    require(!_wcsicmp(regRead(kOwner,kMarker).c_str(),owner.c_str())&&ownedAncestor(owned[i].device.DevInst,adapter.DevInst),"monitor_ownership_changed");
    setFriendlyName(monitors.h,owned[i].device,L"Multitap "+std::to_wstring(i+1));++result.monitorNamesApplied;
  }
  return result;
}
static void namesJson(const Naming& result) {
  std::cout<<std::boolalpha<<"{\"adapterNamed\":"<<result.adapterNamed<<",\"monitorNamesApplied\":"<<result.monitorNamesApplied<<",\"ownedMonitorCount\":"<<result.ownedMonitorCount<<",\"namingError\":"<<result.error<<",\"reason\":\""<<result.reason<<"\",\"state\":"<<statusJson(status())<<"}\n";
}
static fs::path package() { wchar_t p[32768]{}; require(GetModuleFileNameW(nullptr,p,DWORD(std::size(p)))!=0,"module_path_unavailable");return fs::path(p).parent_path()/L"driver"; }
static std::string sha256(const fs::path& p) {
  HANDLE file=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  require(file!=INVALID_HANDLE_VALUE,"driver_file_unavailable");
  BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr; std::array<unsigned char,32> digest{};std::array<unsigned char,65536> block{};DWORD count=0;
  const bool opened=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 && BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
  bool ok=opened;
  if(opened) {
    for(;;) { if(!ReadFile(file,block.data(),DWORD(block.size()),&count,nullptr)){ok=false;break;} if(!count)break; if(BCryptHashData(hash,block.data(),count,0)<0){ok=false;break;} }
    if(ok)ok=BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0)>=0;
  }
  if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);CloseHandle(file); require(ok,"driver_hash_failed");
  static constexpr char hex[]="0123456789abcdef";std::string result;for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
static bool trusted(const fs::path& p) {
  WINTRUST_FILE_INFO file{sizeof(file)};file.pcwszFilePath=p.c_str();WINTRUST_DATA data{sizeof(data)};data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result=WinVerifyTrust(nullptr,&action,&data);data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);return result==ERROR_SUCCESS;
}
static bool catalogMember(const fs::path& catalog,const fs::path& member) {
  HCATADMIN admin=nullptr;
  if(!CryptCATAdminAcquireContext2(&admin,nullptr,BCRYPT_SHA256_ALGORITHM,nullptr,0)) return false;
  HANDLE file=CreateFileW(member.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE){CryptCATAdminReleaseContext(admin,0);return false;}
  DWORD count=0;bool ok=CryptCATAdminCalcHashFromFileHandle2(admin,file,&count,nullptr,0)!=FALSE && count>0 && count<=128;
  std::vector<BYTE> hash(count);
  if(ok)ok=CryptCATAdminCalcHashFromFileHandle2(admin,file,&count,hash.data(),0)!=FALSE;
  if(ok) {
    constexpr wchar_t hex[]=L"0123456789ABCDEF";std::wstring tag;
    for(DWORD i=0;i<count;++i){tag+=hex[hash[i]>>4];tag+=hex[hash[i]&15];}
    WINTRUST_CATALOG_INFO info{};info.cbStruct=sizeof(info);info.pcwszCatalogFilePath=catalog.c_str();info.pcwszMemberFilePath=member.c_str();info.pcwszMemberTag=tag.c_str();info.hMemberFile=file;info.pbCalculatedFileHash=hash.data();info.cbCalculatedFileHash=count;info.hCatAdmin=admin;
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;data.dwUnionChoice=WTD_CHOICE_CATALOG;data.pCatalog=&info;data.dwStateAction=WTD_STATEACTION_VERIFY;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;ok=WinVerifyTrust(nullptr,&action,&data)==ERROR_SUCCESS;
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);
  }
  CloseHandle(file);CryptCATAdminReleaseContext(admin,0);return ok;
}
static void regularPath(const fs::path& path,bool directory=false) {
  const DWORD attributes=GetFileAttributesW(path.c_str());
  require(attributes!=INVALID_FILE_ATTRIBUTES,"driver_file_unavailable");
  require(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"driver_path_reparse_point");
  require(((attributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory,"driver_path_type_invalid");
}
static void verify() {
  const auto dir=package();
  const std::pair<const wchar_t*,const char*> files[]={
    {L"MttVDD.inf","550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d"},
    {L"MttVDD.dll","c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05"},
    {L"mttvdd.cat","08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6"},
    {L"vdd_settings.xml","37b3bf7f87e8938b6ec3ab370272ea0f71ff1640f8061fef4b1b74e833e5efa9"}};
  regularPath(dir,true);
  for(auto [name,hash]:files){const auto p=dir/name;regularPath(p);require(sha256(p)==hash,"driver_hash_mismatch");}
  require(trusted(dir/L"mttvdd.cat")&&trusted(dir/L"MttVDD.dll"),"driver_signature_untrusted");
  require(catalogMember(dir/L"mttvdd.cat",dir/L"MttVDD.inf")&&catalogMember(dir/L"mttvdd.cat",dir/L"MttVDD.dll"),"driver_catalog_membership_invalid");
}
struct PackageLock {
  std::vector<HANDLE> handles;
  explicit PackageLock(const fs::path& dir) {
    try {
      regularPath(dir,true);
      const HANDLE directory=CreateFileW(dir.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
      require(directory!=INVALID_HANDLE_VALUE,"driver_package_lock_failed");handles.push_back(directory);
      for(const auto name:{L"MttVDD.inf",L"MttVDD.dll",L"mttvdd.cat",L"vdd_settings.xml"}) {
        const auto file=dir/name;regularPath(file);
        const HANDLE handle=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(handle!=INVALID_HANDLE_VALUE,"driver_package_lock_failed");handles.push_back(handle);
      }
    } catch(...) {for(const auto handle:handles)CloseHandle(handle);throw;}
  }
  ~PackageLock(){for(const auto handle:handles)CloseHandle(handle);}
  PackageLock(const PackageLock&)=delete;PackageLock& operator=(const PackageLock&)=delete;
};
static std::vector<Display> dualPlan(const std::vector<Display>& existing,const std::vector<std::wstring>& names) {
  require(names.size()==2&&names[0]!=names[1],"virtual_display_count_not_two");
  long long right=0;int top=0;bool primary=false;size_t others=0;
  for(const auto& d:existing) {
    if(std::find(names.begin(),names.end(),d.name)!=names.end()) continue;
    ++others;
    right=std::max(right,static_cast<long long>(d.left)+d.width);
    if(d.primary){top=d.top;primary=true;}
  }
  require(!others||primary,"existing_primary_display_unavailable");
  require(right>=0&&right+3840<=std::numeric_limits<LONG>::max()&&top<=std::numeric_limits<LONG>::max()-1080,"display_layout_out_of_range");
  return {{names[0],int(right),top,1920,1080,!primary,60},{names[1],int(right)+1920,top,1920,1080,false,60}};
}
static bool sameDisplay(const Display& a,const Display& b) {
  return a.name==b.name&&a.left==b.left&&a.top==b.top&&a.width==b.width&&a.height==b.height&&a.primary==b.primary&&a.frequency==b.frequency;
}
static std::vector<Display> topologyPlan(const std::vector<Display>& existing,const std::vector<std::wstring>& names,unsigned count) {
  require((count==1||count==2)&&names.size()==2&&names[0]!=names[1],"invalid_owned_topology");
  const auto primary=std::find_if(existing.begin(),existing.end(),[](const Display& d){return d.primary;});
  require(primary!=existing.end()&&std::count_if(existing.begin(),existing.end(),[](const Display& d){return d.primary;})==1,"existing_primary_display_unavailable");
  std::vector<Display> owned;
  for(const auto& d:existing)if(std::find(names.begin(),names.end(),d.name)!=names.end())owned.push_back(d);
  std::sort(owned.begin(),owned.end(),[](const Display& a,const Display& b){return a.primary!=b.primary?a.primary:a.left!=b.left?a.left<b.left:a.top!=b.top?a.top<b.top:a.name<b.name;});
  if(owned.size()>count)owned.resize(count);
  long long right=0;for(const auto& d:existing)right=std::max(right,static_cast<long long>(d.left)+d.width);
  for(const auto& name:names) {
    if(owned.size()==count)break;
    if(std::any_of(owned.begin(),owned.end(),[&](const Display& d){return d.name==name;}))continue;
    require(right>=0&&right+1920<=std::numeric_limits<LONG>::max()&&primary->top<=std::numeric_limits<LONG>::max()-1080,"display_layout_out_of_range");
    owned.push_back({name,int(right),primary->top,1920,1080,false,60});right+=1920;
  }
  return owned;
}
static void topology(unsigned count,bool dryRun=false) {
  const auto state=status();
  require(!viewportRecoveryPending(state),"viewport_recovery_required");
  require(state.owned&&state.ready&&state.deviceCount==1&&state.foreignDeviceCount==0&&!state.vendorPathConflict,"no_exclusive_owned_virtual_display");
  require(consoleDesktop(),"interactive_console_required");
  require(unlockedDefaultDesktop(),"interactive_desktop_locked");
  require(!state.restartRequired,"reboot_required");
  LONG error=0;const auto names=displayNames(state.id,error);
  require(error==ERROR_SUCCESS&&names.size()==2,"owned_virtual_sources_unavailable");
  const auto before=activeDisplays(),plan=topologyPlan(before,names,count);
  const auto sourceStillOwned=[&](const std::wstring& name) {
    if(!consoleDesktop())return false;
    const auto current=status();if(!current.owned||current.deviceCount!=1||current.foreignDeviceCount||_wcsicmp(current.id.c_str(),state.id.c_str()))return false;
    LONG query=0;const auto currentNames=displayNames(current.id,query);
    return query==ERROR_SUCCESS&&std::find(currentNames.begin(),currentNames.end(),name)!=currentNames.end();
  };
  std::array<DEVMODEW,2> old{},desired{};std::array<bool,2> oldPrimary{},change{};
  for(size_t i=0;i<2;++i) {
    old[i].dmSize=sizeof(DEVMODEW);desired[i].dmSize=sizeof(DEVMODEW);
    const auto prior=std::find_if(before.begin(),before.end(),[&](const Display& d){return d.name==names[i];});
    const auto target=std::find_if(plan.begin(),plan.end(),[&](const Display& d){return d.name==names[i];});
    if(prior!=before.end()) {
      require(EnumDisplaySettingsExW(names[i].c_str(),ENUM_CURRENT_SETTINGS,&old[i],0)!=FALSE,"virtual_display_mode_unavailable");oldPrimary[i]=prior->primary;
    } else {old[i].dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT;}
    if(target==plan.end()) {
      if(prior==before.end())continue;
      require(!prior->primary,"topology_would_disable_primary");
      desired[i]=old[i];desired[i].dmPelsWidth=0;desired[i].dmPelsHeight=0;desired[i].dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT;change[i]=true;
    } else if(prior==before.end()) {
      const auto modes=supportedModes(names[i]);
      require(std::any_of(modes.begin(),modes.end(),[&](const Mode& m){return m.width==1920&&m.height==1080&&m.refreshRate==60;}),"virtual_display_mode_not_supported");
      desired[i].dmPosition={target->left,target->top};desired[i].dmPelsWidth=1920;desired[i].dmPelsHeight=1080;desired[i].dmDisplayFrequency=60;desired[i].dmBitsPerPel=32;
      desired[i].dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY|DM_BITSPERPEL;change[i]=true;
    }
    if(change[i])require(ChangeDisplaySettingsExW(names[i].c_str(),&desired[i],nullptr,CDS_TEST,nullptr)==DISP_CHANGE_SUCCESSFUL,"virtual_display_topology_not_supported");
  }
  if(dryRun) {
    // Validate both count plans without applying either. When already at two
    // sources, the attach plan is a no-op; still test the selected detach.
    const auto single=topologyPlan(before,names,1);
    for(size_t i=0;i<2;++i) {
      const auto enumerated=supportedModes(names[i]);
      require(std::any_of(enumerated.begin(),enumerated.end(),[](const Mode& m){return m.width==1920&&m.height==1080&&m.refreshRate==60;}),"virtual_display_mode_not_supported");
      const auto prior=std::find_if(before.begin(),before.end(),[&](const Display& d){return d.name==names[i];});
      if(prior!=before.end()&&std::none_of(single.begin(),single.end(),[&](const Display& d){return d.name==prior->name;})) {
        require(!prior->primary,"topology_would_disable_primary");
        DEVMODEW detach=old[i];detach.dmPelsWidth=0;detach.dmPelsHeight=0;detach.dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT;
        require(ChangeDisplaySettingsExW(names[i].c_str(),&detach,nullptr,CDS_TEST,nullptr)==DISP_CHANGE_SUCCESSFUL,"virtual_display_topology_not_supported");
      }
    }
    require(state.displays.size()==1||state.displays.size()==2,"virtual_display_not_active_in_session");
    const auto after=activeDisplays();
    require(after.size()==before.size()&&std::all_of(before.begin(),before.end(),[&](const Display& d){return std::any_of(after.begin(),after.end(),[&](const Display& other){return sameDisplay(d,other);});}),"display_topology_changed");
    require(consoleDesktop()&&unlockedDefaultDesktop(),"interactive_console_required");
    std::cout<<"{\"topologySupported\":true,\"ownedSourceCount\":2,\"displayModesApplied\":false,\"state\":"<<statusJson(status())<<"}\n";return;
  }
  std::vector<size_t> changed;
  try {
    const auto immediate=activeDisplays();
    require(immediate.size()==before.size()&&std::all_of(before.begin(),before.end(),[&](const Display& d){return std::any_of(immediate.begin(),immediate.end(),[&](const Display& other){return sameDisplay(d,other);});}),"display_topology_changed");
    // Only the owned source being attached/detached is changed. Keep every
    // active source's geometry, mode and existing primary flag untouched.
    for(size_t i=0;i<2;++i)if(change[i]) {
      require(unlockedDefaultDesktop(),"interactive_desktop_locked");
      require(sourceStillOwned(names[i]),"owned_source_identity_changed");
      changed.push_back(i);require(ChangeDisplaySettingsExW(names[i].c_str(),&desired[i],nullptr,CDS_UPDATEREGISTRY,nullptr)==DISP_CHANGE_SUCCESSFUL,"virtual_display_topology_failed");
    }
    const auto after=activeDisplays();
    for(const auto& prior:before) {
      const bool ours=std::find(names.begin(),names.end(),prior.name)!=names.end();
      const auto expected=std::find_if(plan.begin(),plan.end(),[&](const Display& d){return d.name==prior.name;});
      const auto current=std::find_if(after.begin(),after.end(),[&](const Display& d){return d.name==prior.name;});
      if(ours&&expected==plan.end())require(current==after.end(),"virtual_display_detach_not_applied");
      else require(current!=after.end()&&sameDisplay(prior,*current),"existing_display_changed");
    }
    for(const auto& expected:plan) {
      const auto current=std::find_if(after.begin(),after.end(),[&](const Display& d){return d.name==expected.name;});
      require(current!=after.end()&&sameDisplay(expected,*current),"virtual_display_topology_not_applied");
    }
    require(std::count_if(after.begin(),after.end(),[&](const Display& d){return std::find(names.begin(),names.end(),d.name)!=names.end();})==count,"virtual_display_topology_not_applied");
  } catch(...) {
    bool restored=true;
    for(auto it=changed.rbegin();it!=changed.rend();++it){const auto i=*it;if(!sourceStillOwned(names[i])||ChangeDisplaySettingsExW(names[i].c_str(),&old[i],nullptr,CDS_UPDATEREGISTRY|(oldPrimary[i]?CDS_SET_PRIMARY:0),nullptr)!=DISP_CHANGE_SUCCESSFUL)restored=false;}
    const auto rollback=activeDisplays();
    if(rollback.size()!=before.size()||!std::all_of(before.begin(),before.end(),[&](const Display& d){return std::any_of(rollback.begin(),rollback.end(),[&](const Display& other){return sameDisplay(d,other);});}))restored=false;
    require(restored,"topology_rollback_incomplete");
    throw;
  }
  std::cout<<"{\"topologyApplied\":true,\"activeDisplayCountRequested\":"<<count<<",\"configurationPending\":false,\"state\":"<<statusJson(status())<<"}\n";
}
struct Configuration { bool configured{},pending{}; const char* reason=""; };
static Configuration configure() {
  require(elevated(),"administrator_required");
  const auto state=status();
  require(!viewportRecoveryPending(state),"viewport_recovery_required");
  require(state.owned&&state.deviceCount==1&&state.foreignDeviceCount==0,"no_exclusive_owned_virtual_display");
  if(state.restartRequired)return {false,true,"reboot_required"};
  if(!consoleDesktop()) return {false,true,"interactive_console_required"};
  if(!state.ready) return {false,true,"virtual_display_device_not_ready"};
  LONG error=0;const auto names=displayNames(state.id,error);
  if(error!=ERROR_SUCCESS||names.size()!=2) return {false,true,"virtual_display_count_not_two"};
  const auto before=activeDisplays(),plan=dualPlan(before,names);
  std::array<DEVMODEW,2> old{},desired{};std::array<bool,2> oldPrimary{};
  for(size_t i=0;i<2;++i) {
    old[i].dmSize=sizeof(DEVMODEW);
    require(EnumDisplaySettingsExW(names[i].c_str(),ENUM_CURRENT_SETTINGS,&old[i],0)||EnumDisplaySettingsExW(names[i].c_str(),ENUM_REGISTRY_SETTINGS,&old[i],0),"virtual_display_mode_unavailable");
    const auto prior=std::find_if(before.begin(),before.end(),[&](const Display& d){return d.name==names[i];});
    if(prior==before.end()) {old[i].dmPelsWidth=0;old[i].dmPelsHeight=0;old[i].dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT;}
    else oldPrimary[i]=prior->primary;
    desired[i]=old[i];desired[i].dmPosition={plan[i].left,plan[i].top};desired[i].dmPelsWidth=1920;desired[i].dmPelsHeight=1080;desired[i].dmDisplayFrequency=60;desired[i].dmBitsPerPel=32;
    desired[i].dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY|DM_BITSPERPEL;
    require(ChangeDisplaySettingsExW(names[i].c_str(),&desired[i],nullptr,CDS_TEST,nullptr)==DISP_CHANGE_SUCCESSFUL,"virtual_display_mode_not_supported");
  }
  // Each request names an owned source explicitly. Never use CDS_GLOBAL,
  // SDC_TOPOLOGY_EXTEND, or a null-device registry reset that can rewrite
  // unrelated screens. The two owned sources are placed beyond all others.
  size_t changed=0;
  try {
    for(size_t i=0;i<2;++i) {
      const DWORD flags=CDS_UPDATEREGISTRY|(plan[i].primary?CDS_SET_PRIMARY:0);
      require(ChangeDisplaySettingsExW(names[i].c_str(),&desired[i],nullptr,flags,nullptr)==DISP_CHANGE_SUCCESSFUL,"virtual_display_configure_failed");++changed;
    }
    const auto after=activeDisplays();
    for(const auto& prior:before) {
      if(std::find(names.begin(),names.end(),prior.name)!=names.end()) continue;
      const auto current=std::find_if(after.begin(),after.end(),[&](const Display& d){return d.name==prior.name;});
      require(current!=after.end()&&sameDisplay(prior,*current),"existing_display_changed");
    }
    for(const auto& expected:plan) {
      const auto current=std::find_if(after.begin(),after.end(),[&](const Display& d){return d.name==expected.name;});
      require(current!=after.end()&&sameDisplay(expected,*current),"virtual_display_configuration_not_applied");
    }
  } catch(...) {
    // Undo only requests made to our own sources. A failure is never reported
    // as configured; an actual driver/runtime trial is still required.
    for(size_t i=0;i<changed;++i) ChangeDisplaySettingsExW(names[i].c_str(),&old[i],nullptr,CDS_UPDATEREGISTRY|(oldPrimary[i]?CDS_SET_PRIMARY:0),nullptr);
    throw;
  }
  return {true,false,""};
}
static void configurationJson(const Configuration& result,bool reboot=false) {
  const auto current=status();
  std::cout<<std::boolalpha<<"{\"configured\":"<<result.configured<<",\"configurationPending\":"<<result.pending<<",\"rebootRequired\":"<<(reboot||current.restartRequired)<<",\"reason\":\""<<result.reason<<"\",\"displayCountRequested\":2,\"state\":"<<statusJson(current)<<"}\n";
}
static void planTest() {
  const Display physical{L"physical",-1280,100,2560,1440,true,75};
  const auto plan=dualPlan({physical},{L"owned0",L"owned1"});
  require(plan.size()==2&&plan[0].left==1280&&plan[0].top==100&&plan[1].left==3200&&plan[0].width==1920&&plan[0].height==1080&&plan[0].frequency==60&&!plan[0].primary,"layout_test_failed");
  const auto empty=dualPlan({},{L"owned0",L"owned1"});require(empty[0].primary&&empty[0].left==0&&empty[1].left==1920,"headless_layout_test_failed");
  bool rejected=false;try{dualPlan({},{L"owned0",L"owned0"});}catch(...){rejected=true;}require(rejected,"duplicate_source_test_failed");
  auto noPrimary=physical;noPrimary.primary=false;rejected=false;try{dualPlan({noPrimary},{L"owned0",L"owned1"});}catch(...){rejected=true;}require(rejected,"existing_primary_test_failed");
  const auto parent=[](DEVINST value,DEVINST& next){if(value==11||value==12){next=10;return true;}if(value==21){next=20;return true;}if(value==20){next=1;return true;}return false;};
  require(ownedAncestor(11,10,parent)&&ownedAncestor(12,10,parent)&&!ownedAncestor(21,10,parent)&&!ownedAncestor(1,10,parent),"monitor_ownership_test_failed");
  require(!ownedAncestor(1,10,[](DEVINST value,DEVINST& next){next=value==1?2:1;return true;}),"monitor_ancestry_cycle_test_failed");
  require(!ownedAncestor(1,100,[](DEVINST value,DEVINST& next){next=value+1;return true;}),"monitor_ancestry_bound_test_failed");
  Status fixture;fixture.installed=true;require(std::string(statusReason(fixture,true))=="virtual_display_not_owned","unowned_status_test_failed");
  fixture.owned=true;fixture.foreignDeviceCount=1;require(std::string(statusReason(fixture,true))=="existing_virtual_display_requires_review","foreign_status_test_failed");
  fixture.foreignDeviceCount=0;fixture.vendorPathConflict=true;require(std::string(statusReason(fixture,true))=="shared_vendor_path_conflict","vendor_conflict_status_test_failed");
  fixture.vendorPathConflict=false;fixture.restartRequired=true;require(std::string(statusReason(fixture,true))=="reboot_required","restart_status_test_failed");fixture.restartRequired=false;
  fixture.vendorPathConflict=false;require(std::string(statusReason(fixture,true))=="virtual_display_device_not_ready","problem_status_test_failed");
  fixture.ready=true;require(std::string(statusReason(fixture,false))=="interactive_console_required"&&std::string(statusReason(fixture,true))=="virtual_display_not_active_in_session","console_status_test_failed");
  std::cout<<"{\"layoutTestsPassed\":true,\"ownershipTestsPassed\":true,\"diagnosticTestsPassed\":true,\"operatingSystemChanged\":false,\"virtualDisplayRuntimeTest\":false}\n";
}
static void topologyTest() {
  const std::vector<std::wstring> names{L"owned0",L"owned1"};
  const Display other{L"physical",-1280,100,2560,1440,true,75},a{L"owned0",1280,100,1280,720,false,60},b{L"owned1",2560,100,1920,1080,false,60};
  auto one=topologyPlan({other,a,b},names,1);require(one.size()==1&&sameDisplay(one[0],a),"topology_keep_existing_mode_failed");
  auto two=topologyPlan({other,a},names,2);require(two.size()==2&&sameDisplay(two[0],a)&&two[1].name==L"owned1"&&two[1].left==2560&&two[1].top==100&&!two[1].primary,"topology_add_without_moving_failed");
  auto ownedPrimary=b;ownedPrimary.primary=true;auto nonprimaryOther=other;nonprimaryOther.primary=false;
  one=topologyPlan({nonprimaryOther,a,ownedPrimary},names,1);require(one.size()==1&&sameDisplay(one[0],ownedPrimary),"topology_primary_preservation_failed");
  const auto unchanged=topologyPlan({other,a,b},names,2);require(unchanged.size()==2&&sameDisplay(unchanged[0],a)&&sameDisplay(unchanged[1],b),"topology_noop_mode_preservation_failed");
  bool rejected=false;try{topologyPlan({nonprimaryOther,a,b},names,1);}catch(...){rejected=true;}require(rejected,"topology_missing_primary_not_rejected");
  rejected=false;try{topologyPlan({other,a,b},names,3);}catch(...){rejected=true;}require(rejected,"topology_invalid_count_not_rejected");
  rejected=false;try{topologyPlan({other,a,b},{L"owned0",L"owned0"},1);}catch(...){rejected=true;}require(rejected,"topology_duplicate_sources_not_rejected");
  Status fixture;fixture.installed=fixture.owned=fixture.ready=true;fixture.displays={a};require(!*statusReason(fixture,true),"single_active_source_misreported_pending");
  std::cout<<"{\"topologyPlanTestsPassed\":true,\"operatingSystemChanged\":false,\"virtualDisplayRuntimeTest\":false}\n";
}
static unsigned number(const wchar_t* text,unsigned min,unsigned max) {
  require(text&&*text,"invalid_resize_arguments");unsigned value=0;
  for(const wchar_t* p=text;*p;++p){require(*p>=L'0'&&*p<=L'9'&&value<=max/10,"invalid_resize_arguments");value=value*10+unsigned(*p-L'0');require(value<=max,"invalid_resize_arguments");}
  require(value>=min,"invalid_resize_arguments");return value;
}
static void resize(const std::wstring& name,unsigned width,unsigned height,unsigned refreshRate) {
  require(width%2==0&&height%2==0,"invalid_resize_arguments");
  const auto state=status();
  require(!viewportRecoveryPending(state),"viewport_recovery_required");
  require(state.owned&&state.ready&&state.deviceCount==1&&state.foreignDeviceCount==0,"no_exclusive_owned_virtual_display");
  require(consoleDesktop(),"interactive_console_required");
  const auto target=std::find_if(state.displays.begin(),state.displays.end(),[&](const Display& d){return !_wcsicmp(d.name.c_str(),name.c_str());});
  require(target!=state.displays.end(),"monitor_not_available");
  const auto modes=supportedModes(target->name);
  require(std::any_of(modes.begin(),modes.end(),[&](const Mode& m){return m.width==width&&m.height==height&&m.refreshRate==refreshRate;}),"unsupported_display_mode");
  const auto before=activeDisplays();
  const RECT desiredRect{target->left,target->top,target->left+LONG(width),target->top+LONG(height)};
  for(const auto& other:before) {
    if(other.name==target->name)continue;
    const RECT otherRect{other.left,other.top,other.left+other.width,other.top+other.height};RECT intersection{};
    require(!IntersectRect(&intersection,&desiredRect,&otherRect),"resize_would_overlap_existing_display");
  }
  DEVMODEW old{};old.dmSize=sizeof(old);require(EnumDisplaySettingsExW(target->name.c_str(),ENUM_CURRENT_SETTINGS,&old,0)!=FALSE,"virtual_display_mode_unavailable");
  DEVMODEW desired=old;desired.dmPelsWidth=width;desired.dmPelsHeight=height;desired.dmDisplayFrequency=refreshRate;desired.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY;
  require(ChangeDisplaySettingsExW(target->name.c_str(),&desired,nullptr,CDS_TEST,nullptr)==DISP_CHANGE_SUCCESSFUL,"unsupported_display_mode");
  require(ChangeDisplaySettingsExW(target->name.c_str(),&desired,nullptr,CDS_UPDATEREGISTRY,nullptr)==DISP_CHANGE_SUCCESSFUL,"display_change_failed");
  try {
    const auto after=activeDisplays();
    for(const auto& prior:before) {
      const auto current=std::find_if(after.begin(),after.end(),[&](const Display& d){return d.name==prior.name;});
      require(current!=after.end(),"existing_display_changed");
      if(prior.name==target->name)require(current->left==prior.left&&current->top==prior.top&&current->primary==prior.primary&&current->width==int(width)&&current->height==int(height)&&current->frequency==refreshRate,"display_change_not_applied");
      else require(sameDisplay(prior,*current),"existing_display_changed");
    }
  } catch(...) {ChangeDisplaySettingsExW(target->name.c_str(),&old,nullptr,CDS_UPDATEREGISTRY,nullptr);throw;}
  std::cout<<"{\"resized\":true,\"width\":"<<width<<",\"height\":"<<height<<",\"refreshRate\":"<<refreshRate<<",\"state\":"<<statusJson(status())<<"}\n";
}
#include "viewport.hpp"
#include "mode_catalog.hpp"
static std::wstring logicalMonitorId(const Status&s,const Display&d,size_t fallback){
  // ConnectorInstance is derived from the driver's fixed ConnectorIndex. Its
  // GDI name and randomly generated monitor-container GUID may change on PnP.
  try{const auto mapping=viewport::sources(s.id);for(size_t i=0;i<mapping.size();++i)if(mapping[i].name==d.name)return std::to_wstring(i);}catch(...){}
  return std::to_wstring(fallback); // Legacy status only; viewport support fails.
}
static void removeDevice(HDEVINFO set,SP_DEVINFO_DATA& d) { SP_REMOVEDEVICE_PARAMS p{};p.ClassInstallHeader.cbSize=sizeof(SP_CLASSINSTALL_HEADER);p.ClassInstallHeader.InstallFunction=DIF_REMOVE;p.Scope=DI_REMOVEDEVICE_GLOBAL;require(SetupDiSetClassInstallParamsW(set,&d,&p.ClassInstallHeader,sizeof(p))&&SetupDiCallClassInstaller(DIF_REMOVE,set,&d),"virtual_display_remove_failed"); }
static void install() {
  require(elevated(),"administrator_required");const auto dir=package();PackageLock lock(dir);verify();const auto before=status();
  require(!viewportRecoveryPending(before),"viewport_recovery_required");
  if(before.installed) {require(before.owned&&before.deviceCount==1&&before.foreignDeviceCount==0,"existing_virtual_display_requires_review");applyNames();std::cout<<statusJson(status())<<'\n';return;}
  const auto prior=regRead(kVendor,L"VDDPATH");require(prior.empty(),"existing_virtual_display_configuration");
  require(regRead(kOwner,kMarker).empty(),"stale_virtual_display_ownership_requires_review");
  Devices set{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY,nullptr)};require(set.h!=INVALID_HANDLE_VALUE,"device_list_create_failed");SP_DEVINFO_DATA d{sizeof(d)};bool registered=false;std::wstring registeredId;BOOL reboot=FALSE;
  try {
    regWrite(kVendor,L"VDDPATH",dir.wstring());
    // Stage without DiInstallDriver's global matching-device update/removal
    // behavior. Only DiInstallDevice below binds our newly created instance.
    require(SetupCopyOEMInfW((dir/L"MttVDD.inf").c_str(),nullptr,SPOST_PATH,0,nullptr,0,nullptr,nullptr)!=FALSE,"signed_driver_install_failed");
    require(SetupDiCreateDeviceInfoW(set.h,L"MultitapVirtualDisplay",&GUID_DEVCLASS_DISPLAY,L"Multitap Virtual Display",nullptr,DICD_GENERATE_ID,&d)!=FALSE,"virtual_device_create_failed");
    const wchar_t hardware[]=L"Root\\MttVDD\0\0";
    require(SetupDiSetDeviceRegistryPropertyW(set.h,&d,SPDRP_HARDWAREID,reinterpret_cast<const BYTE*>(hardware),sizeof(hardware))!=FALSE,"virtual_device_hardware_id_failed");
    require(SetupDiCallClassInstaller(DIF_REGISTERDEVICE,set.h,&d)!=FALSE,"virtual_device_register_failed");registered=true;registeredId=instance(set.h,d);regWrite(kOwner,kMarker,registeredId);regWrite(kOwner,L"PackagePath",dir.wstring());
    // Select only the pinned INF for only this newly registered instance.
    // UpdateDriverForPlugAndPlayDevices would target every matching hw-id.
    SP_DEVINSTALL_PARAMS_W parameters{};parameters.cbSize=sizeof(parameters);
    require(SetupDiGetDeviceInstallParamsW(set.h,&d,&parameters)!=FALSE,"device_install_parameters_unavailable");
    parameters.Flags|=DI_ENUMSINGLEINF;
    require(wcscpy_s(parameters.DriverPath,(dir/L"MttVDD.inf").c_str())==0,"driver_path_too_long");
    require(SetupDiSetDeviceInstallParamsW(set.h,&d,&parameters)&&SetupDiBuildDriverInfoList(set.h,&d,SPDIT_COMPATDRIVER),"pinned_driver_selection_failed");
    SP_DRVINFO_DATA_W selected{};selected.cbSize=sizeof(selected);
    const bool found=SetupDiEnumDriverInfoW(set.h,&d,SPDIT_COMPATDRIVER,0,&selected)!=FALSE;
    BOOL boundReboot=FALSE;const bool bound=found&&DiInstallDevice(nullptr,set.h,&d,&selected,0,&boundReboot)!=FALSE;
    SetupDiDestroyDriverInfoList(set.h,&d,SPDIT_COMPATDRIVER);
    require(bound,"virtual_device_driver_bind_failed");reboot=reboot||boundReboot;
  } catch(...) {
    bool removed=!registered;
    if(registered) {try{removeDevice(set.h,d);removed=true;}catch(...) {}}
    if(removed) {
      if(!registeredId.empty()&&regRead(kOwner,kMarker)==registeredId){Reg key;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,kOwner,0,KEY_SET_VALUE,&key.h)==ERROR_SUCCESS){RegDeleteValueW(key.h,kMarker);if(regRead(kOwner,L"PackagePath")==dir.wstring())RegDeleteValueW(key.h,L"PackagePath");}}
      if(regRead(kVendor,L"VDDPATH")==dir.wstring()){Reg key;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,kVendor,0,KEY_SET_VALUE,&key.h)==ERROR_SUCCESS)RegDeleteValueW(key.h,L"VDDPATH");}
    }
    // A failed device removal deliberately retains ownership for recovery.
    // Shared staged driver-store packages are never globally removed.
    throw;
  }
  for(unsigned i=0;i<10&&!status().ready&&!reboot;++i) Sleep(500);
  const auto configuration=reboot?Configuration{false,true,"reboot_required"}:configure();
  applyNames();configurationJson(configuration,reboot!=FALSE);
}
static void uninstall() {
  require(elevated(),"administrator_required");const auto owner=regRead(kOwner,kMarker);require(!owner.empty(),"no_owned_virtual_display");
  Devices set{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,nullptr,nullptr,0)};require(set.h!=INVALID_HANDLE_VALUE,"device_list_unavailable");SP_DEVINFO_DATA d{sizeof(d)};bool found=false;
  for(DWORD i=0;SetupDiEnumDeviceInfo(set.h,i,&d);++i) if(virtualDevice(set.h,d)&&!_wcsicmp(instance(set.h,d).c_str(),owner.c_str())){removeDevice(set.h,d);found=true;break;}
  require(found,"owned_virtual_display_not_found");
  const auto ownedPath=regRead(kOwner,L"PackagePath");
  if(!ownedPath.empty()&&regRead(kVendor,L"VDDPATH")==ownedPath) {Reg key;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,kVendor,0,KEY_SET_VALUE,&key.h)==ERROR_SUCCESS)RegDeleteValueW(key.h,L"VDDPATH");}
  RegDeleteTreeW(HKEY_LOCAL_MACHINE,kOwner);std::cout<<"{\"removed\":true,\"driverStoreRetained\":true}\n";
}
#include "repair.hpp"
static void jsonTest() {
  const std::wstring fixture=L"Microsoft 기본 디스플레이 어댑터 \"quoted\" \\ path\n\t😀";
  const auto encoded=json(fixture);
  require(encoded.find("\\uae30\\ubcf8")!=std::string::npos&&encoded.find("\\ud83d\\ude00")!=std::string::npos,"json_unicode_escape_test_failed");
  require(encoded.find("\\\"quoted\\\"")!=std::string::npos&&encoded.find("\\\\ path\\u000a\\u0009")!=std::string::npos,"json_delimiter_escape_test_failed");
  require(std::all_of(encoded.begin(),encoded.end(),[](unsigned char ch){return ch>=0x20&&ch<=0x7e;}),"json_ascii_output_test_failed");
  require(json(std::wstring{wchar_t(0xd800),L'A',wchar_t(0xdc00)})=="\"\\ufffdA\\ufffd\"","json_unpaired_surrogate_test_failed");
  std::wstring controls;for(wchar_t unit=0;unit<32;++unit)controls+=unit;
  const auto escapedControls=json(controls);
  require(escapedControls.size()==2+32*6&&escapedControls.find("\\u0000")!=std::string::npos&&escapedControls.find("\\u001f")!=std::string::npos,"json_control_escape_test_failed");
  require(display_json::quoteUtf8(utf8(fixture))==encoded,"json_utf8_error_escape_test_failed");
  require(display_json::quoteUtf8(std::string("\xff"))=="\"display_operation_failed\"","json_invalid_utf8_test_failed");
  std::cout<<"{\"jsonOutputTestsPassed\":true,\"asciiEscapedUnicode\":true,\"invalidSurrogateReplacement\":true,\"fixture\":"<<encoded<<",\"operatingSystemChanged\":false,\"virtualDisplayRuntimeTest\":false}\n";
}
int wmain(int argc,wchar_t** argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring command=argc>=2?argv[1]:L"";
    if(command==L"topology"){require(argc==3&&(std::wstring(argv[2])==L"1"||std::wstring(argv[2])==L"2"),"invalid_topology_arguments");topology(argv[2][0]==L'1'?1:2);return 0;}
    if(command==L"topology-status"){require(argc==2,"invalid_topology_arguments");topology(2,true);return 0;}
    if(command==L"resize") {require(argc==6,"invalid_resize_arguments");resize(argv[2],number(argv[3],320,DWORD(INT32_MAX/4)),number(argv[4],180,DWORD(INT32_MAX)),number(argv[5],1,240));return 0;}
    if(command==L"viewport"){require(argc==6,"invalid_viewport_arguments");return viewport::change(argv[2],{number(argv[3],320,DWORD(INT32_MAX/4)),number(argv[4],180,DWORD(INT32_MAX)),number(argv[5],60,60)});}
    if(command==L"viewport-status"){require(argc==2,"invalid_viewport_arguments");return viewport::support();}
    if(command==L"viewport-recover"){require(argc==2,"invalid_viewport_arguments");return viewport::change(L"",{},true);}
    if(command==L"viewport-test"){require(argc==2,"invalid_viewport_arguments");viewport::test();return 0;}
    if(command==L"mode-catalog"){require(argc==2,"invalid_mode_catalog_arguments");return mode_catalog::command();}
    if(command==L"mode-catalog-status"){require(argc==2,"invalid_mode_catalog_arguments");return mode_catalog::command(true);}
    if(command==L"repair-install"){require(argc==3,"invalid_repair_arguments");return repair::outputInstall(argv[2]);}
    require(argc==2,"invalid_command_arguments");
    if(command==L"status")std::cout<<statusJson(status())<<'\n';
    else if(command==L"repair-plan")repair::outputPlan();
    else if(command==L"json-test")jsonTest();
    else if(command==L"verify"){verify();std::cout<<"{\"verified\":true,\"catalogMembershipVerified\":true,\"settingsHashVerified\":true,\"authenticodeSigner\":\"SignPath Foundation\",\"microsoftWhqlVerified\":false}\n";}
    else if(command==L"install")install();else if(command==L"name")namesJson(applyNames());else if(command==L"remove")uninstall();else if(command==L"configure")configurationJson(configure());else if(command==L"plan-test")planTest();else if(command==L"topology-test")topologyTest();
    else if(command==L"warp-test"){Microsoft::WRL::ComPtr<ID3D11Device> device;D3D_FEATURE_LEVEL feature{};const HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&feature,nullptr);require(SUCCEEDED(hr),"warp_unavailable");std::cout<<"{\"warpDeviceCreated\":true,\"featureLevel\":"<<unsigned(feature)<<",\"virtualDisplayRuntimeTest\":false}\n";}
    else throw std::runtime_error("usage_status_verify_install_configure_name_remove_warp-test_plan-test");
    return 0;
  }catch(const std::exception& e){std::cout<<"{\"error\":"<<display_json::quoteUtf8(e.what())<<"}\n";return 1;}
}
