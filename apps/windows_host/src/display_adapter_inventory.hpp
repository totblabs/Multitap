#pragma once
// display-adapters-v1: every Display-class device node of this VM, sorted into
// what Multitap can prove is its own and everything else. Proof, never a name or
// a count (both installers label their node "Multitap Virtual Display Adapter"):
//  - current:   the native selection (native-display.json, protected) names it,
//               and it is ROOT\DISPLAY\<hex> with exactly Root\SunshineVirtualDisplay;
//  - legacy:    ROOT\MULTITAPVIRTUALDISPLAY\NNNN, Root\MttVDD, equal to the
//               installer's owner marker (legacy_display_identity.hpp);
//  - duplicate: a native-shaped node that the setup or this service recorded as
//               created/used by Multitap (HKLM ledger below), but not the selection;
//  - unproven:  same driver identity as ours, no record: possibly an earlier
//               Multitap install from before the ledger, possibly another product;
//  - foreign:   everything else (GPUs, Microsoft Basic, Citrix, other drivers).
// Only current/legacy/duplicate are ours. Automatic cleanup removes legacy and
// duplicate nodes only; unproven ones go only after an administrator confirms
// the exact list in Host settings. Instance IDs stay on this machine: the
// diagnostics carry counts and fixed codes only.
#include "legacy_display_identity.hpp"
#include <algorithm>

namespace host {
inline constexpr std::wstring_view nativeAdapterHardwareId=L"Root\\SunshineVirtualDisplay";
// Written by the Host setup when it registers a native node (setup_pnp.hpp) and by
// the service for the proven current node: value name = instance, REG_DWORD 1.
inline constexpr const wchar_t* nativeAdapterLedgerKey=L"SOFTWARE\\Multitap\\Host\\NativeDisplayNodes";

enum class DisplayAdapterKind:uint32_t {current=0,legacy=1,duplicate=2,unproven=3,foreign=4};
inline const char* displayAdapterKindName(DisplayAdapterKind kind){
  switch(kind){case DisplayAdapterKind::current:return "current";case DisplayAdapterKind::legacy:return "legacy";case DisplayAdapterKind::duplicate:return "duplicate";case DisplayAdapterKind::unproven:return "unproven";default:return "foreign";}
}
struct DisplayAdapterOwnership {std::wstring current,legacyOwner;std::vector<std::wstring> created;};

// ROOT\DISPLAY\ and 4-8 hex digits: the node names DICD_GENERATE_ID gives "Display".
inline bool nativeAdapterInstance(std::wstring_view id){
  constexpr std::wstring_view prefix=L"ROOT\\DISPLAY\\";
  if(id.size()<prefix.size()+4||id.size()>prefix.size()+8||_wcsnicmp(id.data(),prefix.data(),prefix.size())!=0)return false;
  return std::all_of(id.begin()+prefix.size(),id.end(),[](wchar_t ch){return (ch>=L'0'&&ch<=L'9')||(ch>=L'a'&&ch<=L'f')||(ch>=L'A'&&ch<=L'F');});
}
inline bool nativeAdapterShape(const NativeConsoleAdapterIdentity& node){
  return nativeConsoleGuidEqual(node.classGuid,nativeConsoleDisplayClass)&&legacyDisplayTextEqual(node.enumerator,L"ROOT")&&nativeAdapterInstance(node.instanceId)&&
    node.hardwareIds.size()==1&&legacyDisplayTextEqual(node.hardwareIds[0],nativeAdapterHardwareId);
}
inline bool legacyAdapterShape(const NativeConsoleAdapterIdentity& node){
  return nativeConsoleGuidEqual(node.classGuid,nativeConsoleDisplayClass)&&legacyDisplayTextEqual(node.enumerator,L"ROOT")&&legacyDisplayInstance(node.instanceId)&&
    std::any_of(node.hardwareIds.begin(),node.hardwareIds.end(),[](const auto& id){return legacyDisplayTextEqual(id,L"ROOT\\MTTVDD");});
}
// Presence does not decide ownership: a registered phantom of ours is still ours.
inline DisplayAdapterKind classifyDisplayAdapter(const NativeConsoleAdapterIdentity& node,const DisplayAdapterOwnership& owner){
  if(legacyAdapterShape(node))return !owner.legacyOwner.empty()&&legacyDisplayTextEqual(node.instanceId,owner.legacyOwner)?DisplayAdapterKind::legacy:DisplayAdapterKind::unproven;
  if(!nativeAdapterShape(node))return DisplayAdapterKind::foreign;
  if(!owner.current.empty()&&legacyDisplayTextEqual(node.instanceId,owner.current))return DisplayAdapterKind::current;
  const bool recorded=std::any_of(owner.created.begin(),owner.created.end(),[&](const auto& id){return legacyDisplayTextEqual(id,node.instanceId);});
  return recorded?DisplayAdapterKind::duplicate:DisplayAdapterKind::unproven;
}

struct DisplayAdapterExtra {std::wstring instance;DisplayAdapterKind kind=DisplayAdapterKind::foreign;bool present=false,started=false;};
struct DisplayAdapterInventory {
  bool read=false,currentStarted=false;
  uint32_t current=0,legacy=0,duplicate=0,unproven=0,foreign=0;
  std::vector<DisplayAdapterExtra> extras;  // legacy, duplicate and unproven nodes, in enumeration order
};
inline DisplayAdapterInventory summarizeDisplayAdapters(const std::vector<NativeConsoleAdapterIdentity>& nodes,const DisplayAdapterOwnership& owner){
  DisplayAdapterInventory out;out.read=true;
  for(const auto& node:nodes){
    const auto kind=classifyDisplayAdapter(node,owner);
    switch(kind){
      case DisplayAdapterKind::current:++out.current;out.currentStarted=out.currentStarted||(node.present&&node.started);break;
      case DisplayAdapterKind::legacy:++out.legacy;break;
      case DisplayAdapterKind::duplicate:++out.duplicate;break;
      case DisplayAdapterKind::unproven:++out.unproven;break;
      default:++out.foreign;break;
    }
    if(kind==DisplayAdapterKind::legacy||kind==DisplayAdapterKind::duplicate||kind==DisplayAdapterKind::unproven)out.extras.push_back({node.instanceId,kind,node.present,node.started});
  }
  return out;
}
// Why automatic cleanup is not removing a proven extra now ("" when it may).
// The runtime must have its own adapter (exactly one current node, started), no
// stream or display operation may be running, and no saved display state may be
// waiting to be restored (it can still name an older adapter's outputs).
inline const char* displayAdapterCleanupBlocker(const DisplayAdapterInventory& inventory,bool providerConfirmed,bool idle,bool statePresent){
  if(!inventory.read)return "inventory_unavailable";
  if(!inventory.legacy&&!inventory.duplicate)return "";
  if(!providerConfirmed||inventory.current!=1||!inventory.currentStarted)return "current_not_ready";
  if(statePresent)return "display_state_pending";
  if(!idle)return "busy";
  return "";
}
// The last automatic cleanup of this service run.
enum class DisplayAdapterCleanup:uint32_t {none=0,removed=1,pending=2,failed=3,refused=4};
inline const char* displayAdapterCleanupName(DisplayAdapterCleanup value){
  switch(value){case DisplayAdapterCleanup::removed:return "removed";case DisplayAdapterCleanup::pending:return "pending_restart";case DisplayAdapterCleanup::failed:return "failed";case DisplayAdapterCleanup::refused:return "refused";default:return "none";}
}
// Fixed codes and counts only (Host diagnostics and settings status).
inline std::string displayAdapterInventoryJson(const DisplayAdapterInventory& inventory,const char* blocker,DisplayAdapterCleanup last){
  static const std::set<std::string> blockers{"","inventory_unavailable","current_not_ready","display_state_pending","busy"};
  if(!inventory.read)return "{\"read\":false}";
  return "{\"read\":true,\"current\":"+std::to_string(std::min<uint32_t>(inventory.current,64))+",\"legacy\":"+std::to_string(std::min<uint32_t>(inventory.legacy,64))+
    ",\"duplicate\":"+std::to_string(std::min<uint32_t>(inventory.duplicate,64))+",\"unproven\":"+std::to_string(std::min<uint32_t>(inventory.unproven,64))+
    ",\"foreign\":"+std::to_string(std::min<uint32_t>(inventory.foreign,64))+",\"currentStarted\":"+(inventory.currentStarted?"true":"false")+
    ",\"blocker\":"+jsonQuote(blockers.count(blocker?blocker:"")?std::string(blocker?blocker:""):"inventory_unavailable")+",\"cleanup\":\""+displayAdapterCleanupName(last)+"\"}";
}
// Settings status only (this machine): the exact nodes Host settings can offer to
// remove, so the administrator confirms what goes. Never current or foreign.
inline std::string displayAdapterExtrasJson(const DisplayAdapterInventory& inventory){
  std::string out="[";
  for(const auto& extra:inventory.extras){
    if(out.size()>1)out+=',';
    out+="{\"id\":"+jsonQuote(utf8(extra.instance))+",\"kind\":\""+displayAdapterKindName(extra.kind)+"\",\"present\":"+(extra.present?"true":"false")+",\"started\":"+(extra.started?"true":"false")+'}';
  }
  return out+']';
}

// ---- Windows ----------------------------------------------------------------
inline std::optional<std::vector<wchar_t>> displayAdapterProperty(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD key,DWORD wanted){
  DWORD type=0,bytes=0;SetupDiGetDeviceRegistryPropertyW(set,&device,key,&type,nullptr,0,&bytes);
  if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||type!=wanted||bytes<sizeof(wchar_t)||bytes>8192||bytes%sizeof(wchar_t))return {};
  std::vector<wchar_t> value(bytes/sizeof(wchar_t));DWORD actual=0;
  if(!SetupDiGetDeviceRegistryPropertyW(set,&device,key,&type,reinterpret_cast<PBYTE>(value.data()),bytes,&actual)||type!=wanted||actual!=bytes||value.back()!=L'\0')return {};
  return value;
}
// One node's identity from the live device (no DIGCF_PRESENT: phantoms count).
inline std::optional<NativeConsoleAdapterIdentity> displayAdapterNode(HDEVINFO set,SP_DEVINFO_DATA& device){
  NativeConsoleAdapterIdentity node;node.classGuid=device.ClassGuid;
  wchar_t id[201]{};DWORD length=0;
  if(!SetupDiGetDeviceInstanceIdW(set,&device,id,DWORD(std::size(id)),&length)||length<2||length>std::size(id))return {};
  node.instanceId.assign(id,length-1);
  if(const auto enumerator=displayAdapterProperty(set,device,SPDRP_ENUMERATOR_NAME,REG_SZ))node.enumerator.assign(enumerator->data(),enumerator->size()-1);
  if(const auto hardware=displayAdapterProperty(set,device,SPDRP_HARDWAREID,REG_MULTI_SZ))
    for(size_t at=0;at<hardware->size()&&(*hardware)[at]&&node.hardwareIds.size()<16;){
      const auto end=std::find(hardware->begin()+at,hardware->end(),L'\0');if(end==hardware->end())break;
      node.hardwareIds.emplace_back(hardware->data()+at,size_t(end-(hardware->begin()+at)));at=size_t(end-hardware->begin())+1;
    }
  DEVINST instance=0;ULONG flags=0,problem=0;
  node.present=CM_Locate_DevNodeW(&instance,node.instanceId.data(),CM_LOCATE_DEVNODE_NORMAL)==CR_SUCCESS;
  node.started=node.present&&CM_Get_DevNode_Status(&flags,&problem,instance,0)==CR_SUCCESS&&(flags&DN_STARTED)&&!problem;
  return node;
}
inline std::optional<std::vector<NativeConsoleAdapterIdentity>> readDisplayAdapterNodes(){
  struct Set {HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}} set{SetupDiGetClassDevsW(&nativeConsoleDisplayClass,nullptr,nullptr,0)};
  if(set.value==INVALID_HANDLE_VALUE)return {};
  std::vector<NativeConsoleAdapterIdentity> nodes;
  for(DWORD index=0;index<256;++index){
    SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
    if(!SetupDiEnumDeviceInfo(set.value,index,&device))return GetLastError()==ERROR_NO_MORE_ITEMS?std::optional(nodes):std::nullopt;
    if(const auto node=displayAdapterNode(set.value,device))nodes.push_back(*node);else return {};
  }
  return {};  // more nodes than any VM has: not a trustworthy listing
}
inline std::vector<std::wstring> readNativeAdapterLedger(){
  std::vector<std::wstring> out;HKEY key=nullptr;
  if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,nativeAdapterLedgerKey,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key)!=ERROR_SUCCESS)return out;
  for(DWORD index=0;index<64;++index){
    wchar_t name[64]{};DWORD length=DWORD(std::size(name)),type=0,data=0,bytes=sizeof(data);
    if(RegEnumValueW(key,index,name,&length,nullptr,&type,reinterpret_cast<BYTE*>(&data),&bytes)!=ERROR_SUCCESS)break;
    if(type==REG_DWORD&&data==1&&nativeAdapterInstance(name))out.emplace_back(name);
  }
  RegCloseKey(key);return out;
}
inline bool recordNativeAdapterNode(const std::wstring& instance){
  if(!nativeAdapterInstance(instance))return false;
  const DWORD one=1;return RegSetKeyValueW(HKEY_LOCAL_MACHINE,nativeAdapterLedgerKey,instance.c_str(),REG_DWORD,&one,sizeof(one))==ERROR_SUCCESS;
}
inline void forgetNativeAdapterNode(const std::wstring& instance){
  if(nativeAdapterInstance(instance))RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,nativeAdapterLedgerKey,instance.c_str());
}
inline DisplayAdapterOwnership readDisplayAdapterOwnership(bool providerConfirmed,const std::wstring& selection){
  DisplayAdapterOwnership owner;
  if(providerConfirmed&&nativeAdapterInstance(selection))owner.current=selection;
  try{owner.legacyOwner=legacyOwnerInstanceFromRegistry();}catch(...){ }
  try{owner.created=readNativeAdapterLedger();}catch(...){ }
  return owner;
}
// Removes one node only if, read again from the live device right now, it is still
// one of [allowed] for [owner]. removed: the node is gone; pending: Windows still
// lists it (a restart may finish it). The driver package stays in the driver store.
inline DisplayAdapterCleanup removeDisplayAdapter(const std::wstring& instance,const DisplayAdapterOwnership& owner,std::initializer_list<DisplayAdapterKind> allowed){
  struct Set {HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}} set{SetupDiCreateDeviceInfoList(&nativeConsoleDisplayClass,nullptr)};
  if(set.value==INVALID_HANDLE_VALUE)return DisplayAdapterCleanup::failed;
  SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
  if(!SetupDiOpenDeviceInfoW(set.value,instance.c_str(),nullptr,0,&device))return GetLastError()==ERROR_NO_SUCH_DEVINST?DisplayAdapterCleanup::removed:DisplayAdapterCleanup::failed;
  const auto node=displayAdapterNode(set.value,device);
  if(!node||!legacyDisplayTextEqual(node->instanceId,instance))return DisplayAdapterCleanup::refused;
  const auto kind=classifyDisplayAdapter(*node,owner);
  if(kind==DisplayAdapterKind::current||kind==DisplayAdapterKind::foreign||std::find(allowed.begin(),allowed.end(),kind)==allowed.end())return DisplayAdapterCleanup::refused;
  if(!SetupDiCallClassInstaller(DIF_REMOVE,set.value,&device))return DisplayAdapterCleanup::failed;
  DEVINST gone=0;std::wstring id=instance;
  return CM_Locate_DevNodeW(&gone,id.data(),CM_LOCATE_DEVNODE_PHANTOM)==CR_NO_SUCH_DEVNODE?DisplayAdapterCleanup::removed:DisplayAdapterCleanup::pending;
}
// The overall result of several removals: any failure, else any pending, else removed.
inline DisplayAdapterCleanup combineDisplayAdapterCleanup(DisplayAdapterCleanup a,DisplayAdapterCleanup b){
  const auto rank=[](DisplayAdapterCleanup v){switch(v){case DisplayAdapterCleanup::failed:return 4;case DisplayAdapterCleanup::refused:return 3;case DisplayAdapterCleanup::pending:return 2;case DisplayAdapterCleanup::removed:return 1;default:return 0;}};
  return rank(a)>=rank(b)?a:b;
}
}  // namespace host
