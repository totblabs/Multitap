#pragma once
// The driver package one existing Multitap-owned virtual display node is bound to
// (read only): which published INF, its SHA-256 and version, and whether the node
// is in the Host's protected ledger. Nothing here changes a device.
#include "setup_payload.hpp"
#include <string>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
namespace multitap::pnp {
inline constexpr const char* Driver164InfSha="3212b43e7dc20cf36046421e8fd57910d0bb5fc5e5c67c920bbfa24d6f10f95c";
inline constexpr const char* Driver165InfSha="c5e5abf485e35d5eff4bf8696d50f6ceb4d42f7c09e3562eeba53557b510d32a";
struct UpgradeBinding {
 bool present=false,compatible=false,ledgerOwned=false;
 std::string infSha,version;
 std::wstring publishedInf;
 unsigned long flags=0,problem=0xffffffffUL;
};
inline bool publishedInfName(const std::wstring& value){
 if(value.size()<8||value.size()>16||value.substr(0,3)!=L"oem"||value.substr(value.size()-4)!=L".inf")return false;
 for(size_t n=3;n+4<value.size();++n)if(value[n]<L'0'||value[n]>L'9')return false;
 return true;
}
class ExactDeviceUpgrade {
 public:
 static UpgradeBinding inspect(const std::wstring& instance){
  Set set(SetupDiCreateDeviceInfoList(nullptr,nullptr));if(!set.valid())return {};
  SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
  if(!SetupDiOpenDeviceInfoW(set.get(),instance.c_str(),nullptr,0,&device))return {};
  return binding(set.get(),device,instance);
 }
 private:
 class Set{public:explicit Set(HDEVINFO value):value_(value){}~Set(){if(valid())SetupDiDestroyDeviceInfoList(value_);}bool valid()const{return value_!=INVALID_HANDLE_VALUE;}HDEVINFO get()const{return value_;}private:HDEVINFO value_;};
 static std::optional<std::wstring> property(HDEVINFO set,SP_DEVINFO_DATA& device,const DEVPROPKEY& key){
  DEVPROPTYPE type=0;DWORD size=0;std::array<BYTE,4096> bytes{};
  if(!SetupDiGetDevicePropertyW(set,&device,&key,&type,bytes.data(),DWORD(bytes.size()),&size,0)||type!=DEVPROP_TYPE_STRING||size<2||size>bytes.size()||(size&1)||bytes[size-1]||bytes[size-2])return {};
  std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data()),size/2-1);if(text.find(L'\0')!=std::wstring::npos)return {};return text;
 }
 static bool ledger(const std::wstring& instance){
  HKEY key=nullptr;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Multitap\\Host\\NativeDisplayNodes",REG_OPTION_OPEN_LINK,KEY_QUERY_VALUE|READ_CONTROL,&key)!=ERROR_SUCCESS)return false;
  struct Close{HKEY key;~Close(){RegCloseKey(key);}} close{key};
  PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(GetSecurityInfo(key,SE_REGISTRY_KEY,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
  struct Free{PSECURITY_DESCRIPTOR value;~Free(){LocalFree(value);}} free{descriptor};
  if(!owner||!dacl||!(payload::sidText(owner,L"S-1-5-18")||payload::sidText(owner,L"S-1-5-32-544")))return false;
  constexpr ACCESS_MASK writes=KEY_SET_VALUE|KEY_CREATE_SUB_KEY|DELETE|WRITE_DAC|WRITE_OWNER|GENERIC_WRITE|GENERIC_ALL;
  for(DWORD n=0;n<dacl->AceCount;++n){void* raw=nullptr;if(!GetAce(dacl,n,&raw))return false;auto* header=static_cast<ACE_HEADER*>(raw);if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)return false;auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);auto* sid=reinterpret_cast<PSID>(&ace->SidStart);if((ace->Mask&writes)&&!(payload::sidText(sid,L"S-1-5-18")||payload::sidText(sid,L"S-1-5-32-544")||payload::sidText(sid,L"S-1-3-0")))return false;}
  DWORD value=0,size=sizeof(value);return RegGetValueW(key,nullptr,instance.c_str(),RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS&&size==sizeof(value)&&value==1;
 }
 static std::string infHash(const std::wstring& name){
  if(!publishedInfName(name))return {};
  wchar_t windows[MAX_PATH]{};const UINT size=GetWindowsDirectoryW(windows,MAX_PATH);if(!size||size>=MAX_PATH)return {};
  const std::wstring path=std::wstring(windows)+L"\\INF\\"+name;
  try{payload::Handle file(CreateFileW(path.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));LARGE_INTEGER length{};if(!file||(payload::attributesOf(file.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(file.get(),&length)||length.QuadPart<1||length.QuadPart>65536)return {};payload::protectedDirectory(file.get());const auto hash=payload::hashOf(file.get(),static_cast<unsigned long long>(length.QuadPart));std::string text;static constexpr char hex[]="0123456789abcdef";for(const auto b:hash){text+=hex[b>>4];text+=hex[b&15];}return text;}catch(...){return {};}
 }
 static UpgradeBinding binding(HDEVINFO set,SP_DEVINFO_DATA& device,const std::wstring& instance){
  UpgradeBinding result;wchar_t id[512]{};DWORD needed=0;
  if(!SetupDiGetDeviceInstanceIdW(set,&device,id,512,&needed)||!payload::sameText(id,instance))return result;
  DEVINST node=0;std::wstring located=instance;if(CM_Locate_DevNodeW(&node,located.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS)return result;
  result.present=true;
  DWORD type=0,size=0;std::array<BYTE,4096> hardware{};
  const GUID display{0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
  const std::wstring wanted=L"Root\\SunshineVirtualDisplay";
  if(device.ClassGuid==display&&SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_HARDWAREID,&type,hardware.data(),DWORD(hardware.size()),&size)&&type==REG_MULTI_SZ&&size==(wanted.size()+2)*sizeof(wchar_t)){
   const auto* text=reinterpret_cast<const wchar_t*>(hardware.data());result.compatible=text[wanted.size()]==0&&text[wanted.size()+1]==0&&payload::sameText(std::wstring(text,wanted.size()),wanted);
  }
  result.ledgerOwned=ledger(instance);
  // Windows DEVPKEY_Device_DriverInfPath / DriverVersion. Local constexpr
  // definitions avoid external GUID storage requirements in each setup target.
  constexpr DEVPROPKEY driverInfPath{{0xa8b865dd,0x2e3d,0x4094,{0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},5};
  constexpr DEVPROPKEY driverVersion{{0xa8b865dd,0x2e3d,0x4094,{0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},3};
  const auto inf=property(set,device,driverInfPath),version=property(set,device,driverVersion);
  if(inf)result.publishedInf=*inf;if(version)result.version=payload::utf8(*version);result.infSha=infHash(result.publishedInf);
  ULONG flags=0,problem=0;if(CM_Get_DevNode_Status(&flags,&problem,node,0)==CR_SUCCESS){result.flags=flags;result.problem=problem;}
  return result;
 }
};
}
