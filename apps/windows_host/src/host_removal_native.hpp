#pragma once
// The machine side of host-removal-v1 (host_removal.hpp holds every decision).
// Run by Windows Installer as SYSTEM while it removes the Host:
//   MultitapHostTool.exe --product-removal-cleanup --remove=ALL --upgrading= --product={...}
// Device nodes, the driver package, the two certificate entries and the display
// records are deleted, each only as host_removal.hpp decides. Nothing is added
// to a trust store, no Windows policy or security setting is read or changed,
// no driver file is modified, and Windows is never restarted.
#include "host_removal.hpp"
#include "host_prerequisites_native.hpp"
#include "installed_host_layout.hpp"
#include "native_display_trust.hpp"
#include "operational_log.hpp"
#include <cctype>
#include <cwctype>

namespace host::removal {
// What the driver's nodes and packages are recognised by.
inline constexpr const wchar_t* DriverHardwareId=L"Root\\SunshineVirtualDisplay";
inline constexpr const char* DriverName="SunshineVirtualDisplay";
// Windows DEVPKEY_Device_DriverInfPath (setup_pnp_upgrade.hpp reads the same key).
inline constexpr DEVPROPKEY DriverInfPathKey{{0xa8b865dd,0x2e3d,0x4094,{0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},5};

// The product code of the install this program belongs to: the marker beside
// bin\, in a folder chain only SYSTEM, Administrators or Windows servicing can
// change. "" when this is not such an install.
inline std::wstring installedProductCode() noexcept {
  try{
    const auto bin=std::filesystem::path(executablePath()).parent_path();const auto root=bin.parent_path();const auto marker=root/L"multitap-host.json";
    if(_wcsicmp(bin.filename().c_str(),L"bin")!=0)return {};
    for(const auto& path:{root,bin,marker})if(!trustedInstalledPath(path))return {};
    auto bytes=readInstalledMarker(marker);
    if(bytes.empty()||bytes.size()>8192)return {};
    if(bytes.size()>=3&&uint8_t(bytes[0])==0xef&&uint8_t(bytes[1])==0xbb&&uint8_t(bytes[2])==0xbf)bytes.erase(0,3);
    const auto metadata=JsonParser::parse(bytes);
    if(metadata.get("product").string()!="MultitapHost"||metadata.get("version").string()!=MULTITAP_HOST_VERSION_UTF8)return {};
    return wide(metadata.get("productCode").string());
  }catch(...){return {};}
}

class Machine final:public Access {
 public:
  Machine(){
    // One snapshot of what Multitap owns, taken before anything is removed.
    try{prerequisites::Machine settings;const auto selection=settings.selection();if(selection.valid)owner_.created.push_back(selection.instance);}catch(...){ }
    try{for(const auto& id:readNativeAdapterLedger())if(!known(id))owner_.created.push_back(id);}catch(...){ }
    try{owner_.legacyOwner=legacyOwnerInstanceFromRegistry();}catch(...){ }
  }
  Machine(const Machine&)=delete;Machine& operator=(const Machine&)=delete;

  bool installerRunning()override{
    // host_installer.cpp holds this for its whole run (install and update), created with a
    // SYSTEM/Administrators list. A lock of that name that is not ours (any user can create
    // Global\ objects first) is ignored: it must never keep the certificate or driver behind.
    if(lockSeen(L"Global\\MultitapHostInstaller")==NamedLock::ours)return true;
    // Host settings may be installing the device right now (host_prerequisites_native.hpp).
    try{lock_=nativeConnectionLock(L"Global\\MultitapHost.NativeDisplayInstall",0);}
    catch(...){
      // nativeConnectionLock refuses a lock with another owner or list: that one is not ours either.
      if(lockSeen(L"Global\\MultitapHost.NativeDisplayInstall")==NamedLock::foreign)return false;
      return true;
    }
    return !lock_;
  }
  bool foreignLockIgnored()override{return foreign_;}
  NamedLock lockSeen(const wchar_t* name){
    const HANDLE lock=OpenMutexW(SYNCHRONIZE|READ_CONTROL,FALSE,name);
    const DWORD error=lock?ERROR_SUCCESS:GetLastError();
    bool administrative=false;
    if(lock){PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      if(GetSecurityInfo(lock,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor)==ERROR_SUCCESS){administrative=hostAdministrativeSid(owner);LocalFree(descriptor);}
      CloseHandle(lock);}
    const auto seen=judgeNamedLock(lock||error!=ERROR_FILE_NOT_FOUND,lock!=nullptr,administrative);
    if(seen==NamedLock::foreign)foreign_=true;
    return seen;
  }
  bool serviceStopped()override{
    const SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!manager)return false;
    bool stopped=false;
    if(const SC_HANDLE service=OpenServiceW(manager,L"MultitapHost",SERVICE_QUERY_STATUS)){
      SERVICE_STATUS status{};stopped=QueryServiceStatus(service,&status)&&status.dwCurrentState==SERVICE_STOPPED;
      CloseServiceHandle(service);
    }else stopped=GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST;
    CloseServiceHandle(manager);return stopped;
  }
  // What the service leaves when it stopped normally: no saved stream layout
  // (display-v2.state), a stream profile that was restored, outputs stopped.
  bool displayAtRest()override{
    try{
      const auto directory=machineDirectory()/L"private";
      if(exists(directory/L"display-v2.state")!=Existence::absent)return false;
      const auto profile=phase(directory/L"native-stream-profile.json"),runtime=phase(directory/L"native-display-runtime.json");
      return (profile=="absent"||profile=="restored")&&(runtime=="absent"||runtime=="stopped");
    }catch(...){return false;}
  }
  std::vector<Node> ownedNodes()override{
    std::vector<Node> out;
    for(const auto& id:owner_.created)out.push_back({id,false});
    if(!owner_.legacyOwner.empty())out.push_back({owner_.legacyOwner,true});
    return out;
  }
  Presence inspect(const Node& node)override{
    try{
      const auto nodes=readDisplayAdapterNodes();if(!nodes)return Presence::unknown;
      const auto found=std::find_if(nodes->begin(),nodes->end(),[&](const auto& each){return legacyDisplayTextEqual(each.instanceId,node.instance);});
      if(found==nodes->end()){
        // Not a display device. Gone altogether, or the ID names something else now.
        DEVINST located=0;std::wstring id=node.instance;
        const auto result=CM_Locate_DevNodeW(&located,id.data(),CM_LOCATE_DEVNODE_PHANTOM);
        return result==CR_NO_SUCH_DEVNODE?Presence::absent:result==CR_SUCCESS?Presence::foreign:Presence::unknown;
      }
      const auto kind=classifyDisplayAdapter(*found,owner_);
      return kind==(node.legacy?DisplayAdapterKind::legacy:DisplayAdapterKind::duplicate)?Presence::present:Presence::foreign;
    }catch(...){return Presence::unknown;}
  }
  void removeNode(const Node& node)override{
    // Read again from the live device and removed only if it is still ours.
    try{(void)removeDisplayAdapter(node.instance,owner_,{node.legacy?DisplayAdapterKind::legacy:DisplayAdapterKind::duplicate});}catch(...){ }
  }
  void forgetNode(const Node& node)override{
    try{if(node.legacy)clearLegacyDisplayOwnerRecord();else forgetNativeAdapterNode(node.instance);}catch(...){ }
  }
  std::optional<std::vector<std::wstring>> packages()override{
    std::vector<std::wstring> out;
    const auto pinned=[](const std::string& hash){return hash==multitap::pnp::Driver165InfSha||hash==multitap::pnp::Driver164InfSha;};
    if(!publishedInfs([&](const std::wstring& name,const std::string& bytes){if(pinned(sha256(bytes)))out.push_back(name);return true;}))return std::nullopt;
    return out;
  }
  std::optional<unsigned> devicesUsing(const std::wstring& package)override{
    unsigned count=0;
    if(!devices([&](HDEVINFO set,SP_DEVINFO_DATA& device){
      DEVPROPTYPE type=0;DWORD size=0;wchar_t name[64]{};
      if(SetupDiGetDevicePropertyW(set,&device,&DriverInfPathKey,&type,reinterpret_cast<BYTE*>(name),sizeof(name)-sizeof(wchar_t),&size,0)&&type==DEVPROP_TYPE_STRING&&_wcsicmp(name,package.c_str())==0)++count;
    }))return std::nullopt;
    return count;
  }
  PackageRemoval removePackage(const std::wstring& package)override{
    if(!multitap::pnp::publishedInfName(package))return PackageRemoval::failed;
    // No SUOI_FORCEDELETE: Windows refuses a package a device is bound to.
    if(SetupUninstallOEMInfW(package.c_str(),0,nullptr))return PackageRemoval::removed;
    return GetLastError()==ERROR_INF_IN_USE_BY_DEVICES?PackageRemoval::inUse:PackageRemoval::failed;
  }
  std::optional<bool> publisherNeeded()override{
    bool needed=false;
    if(!devices([&](HDEVINFO set,SP_DEVINFO_DATA& device){
      if(const auto hardware=displayAdapterProperty(set,device,SPDRP_HARDWAREID,REG_MULTI_SZ))
        for(size_t at=0;at<hardware->size()&&(*hardware)[at];){
          const auto end=std::find(hardware->begin()+at,hardware->end(),L'\0');if(end==hardware->end())break;
          if(legacyDisplayTextEqual(std::wstring_view(hardware->data()+at,size_t(end-(hardware->begin()+at))),DriverHardwareId))needed=true;
          at=size_t(end-hardware->begin())+1;
        }
    }))return std::nullopt;
    if(needed)return true;
    if(!publishedInfs([&](const std::wstring&,const std::string& bytes){if(namesDriver(bytes))needed=true;return true;}))return std::nullopt;
    return needed;
  }
  std::vector<std::string> trustAdded()override{
    // An earlier Host recorded an added certificate only in the journal.
    (void)backfillNativeDisplayTrustFromJournal(prerequisites::PublisherThumbprint);
    return readNativeDisplayTrust(prerequisites::PublisherThumbprint);
  }
  bool removeCertificate(const std::string& store)override{
    try{
      const auto* name=prerequisites::publisherStoreName(store);if(!name)return false;
      {prerequisites::CertificateStore opened(name,true);
       // CertDeleteCertificateFromStore frees the context it is given.
       if(const auto found=opened.publisher())CertDeleteCertificateFromStore(found);}
      return !certificateInMachineStore(store,prerequisites::PublisherThumbprint);
    }catch(...){return false;}
  }
  void forgetTrust(const std::string& store)override{forgetNativeDisplayTrust(store);}
  bool removeDisplayState()override{
    try{
      const auto directory=machineDirectory();
      if(GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES)return true;
      // Fixed names in folders only administrators can change, plain files only.
      if(!trustedMachineFile(directory))return false;
      bool clean=true;
      for(const auto* name:{L"native-display.json",L"native-display-state.json"})clean=removeFile(directory/name)&&clean;
      const auto records=directory/L"private";
      if(GetFileAttributesW(records.c_str())==INVALID_FILE_ATTRIBUTES)return clean;
      if(!trustedMachineFile(records))return false;
      for(const auto* name:{L"native-stream-profile.json",L"native-stream-launch.json",L"native-stream-viewport-commit.json",L"native-stream-reclaim.json",L"native-stream-expired-console.json",
                            L"native-stream-retirement.json",L"native-display-baseline.json",L"native-display-runtime.json",L"native-display.dpapi",L"display-v2.state"})
        clean=removeFile(records/name)&&clean;
      return clean;
    }catch(...){return false;}
  }

 private:
  DisplayAdapterOwnership owner_;  // `current` stays empty: the selection's node is listed with the ledger's
  ConnectionGuardPtr lock_;
  bool foreign_=false;
  bool known(const std::wstring& id)const{return std::any_of(owner_.created.begin(),owner_.created.end(),[&](const auto& each){return legacyDisplayTextEqual(each,id);});}
  enum class Existence {absent,present,unknown};
  static Existence exists(const std::filesystem::path& path){
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return Existence::present;
    const DWORD error=GetLastError();
    return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?Existence::absent:Existence::unknown;
  }
  // "absent", the record's phase, or "unknown" for anything that cannot be read.
  static std::string phase(const std::filesystem::path& path){
    const auto there=exists(path);
    if(there==Existence::absent)return "absent";
    if(there!=Existence::present||!trustedMachineFile(path)||std::filesystem::file_size(path)>65536)return "unknown";
    std::ifstream file(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    const auto* text=std::get_if<std::string>(&JsonParser::parse(bytes).get("phase").value);
    return text&&!text->empty()?*text:std::string("unknown");
  }
  static bool removeFile(const std::filesystem::path& path){
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
    if(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))return false;
    return DeleteFileW(path.c_str())!=FALSE;
  }
  // Every device node Windows knows, present or not. False: the list is incomplete.
  template<class Visit> static bool devices(Visit&& visit){
    struct Set {HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}} set{SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES)};
    if(set.value==INVALID_HANDLE_VALUE)return false;
    for(DWORD index=0;index<16384;++index){
      SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
      if(!SetupDiEnumDeviceInfo(set.value,index,&device))return GetLastError()==ERROR_NO_MORE_ITEMS;
      visit(set.value,device);
    }
    return false;
  }
  // Every published third-party INF (%windir%\INF\oemNN.inf) with its bytes.
  // False: the folder could not be listed or a file could not be read.
  template<class Visit> static bool publishedInfs(Visit&& visit){
    wchar_t windows[MAX_PATH]{};const UINT length=GetWindowsDirectoryW(windows,MAX_PATH);if(!length||length>=MAX_PATH)return false;
    const auto folder=std::filesystem::path(windows)/L"INF";
    WIN32_FIND_DATAW entry{};const HANDLE find=FindFirstFileW((folder/L"oem*.inf").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    struct Close {HANDLE value;~Close(){FindClose(value);}} close{find};
    do{
      std::wstring name=entry.cFileName;for(auto& ch:name)ch=wchar_t(towlower(ch));
      if(!multitap::pnp::publishedInfName(name)||(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))continue;
      if(entry.nFileSizeHigh||entry.nFileSizeLow>(4u<<20))return false;
      std::ifstream file(folder/name,std::ios::binary);if(!file)return false;
      const std::string bytes((std::istreambuf_iterator<char>(file)),{});
      if(bytes.size()!=entry.nFileSizeLow)return false;
      visit(name,bytes);
    }while(FindNextFileW(find,&entry));
    return GetLastError()==ERROR_NO_MORE_FILES;
  }
  static std::string sha256(const std::string& bytes){
    BYTE digest[32]{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),ULONG(bytes.size()),digest,sizeof(digest))!=0)return {};
    static constexpr char hex[]="0123456789abcdef";std::string text;
    for(const BYTE value:digest){text+=hex[value>>4];text+=hex[value&15];}
    return text;
  }
  // An INF is text, ANSI or UTF-16: the driver's name in either form.
  static bool namesDriver(const std::string& bytes){
    const std::string narrow=DriverName;std::string wideName;for(const char ch:narrow){wideName+=ch;wideName+='\0';}
    const auto contains=[&](const std::string& needle){
      return std::search(bytes.begin(),bytes.end(),needle.begin(),needle.end(),[](char a,char b){return std::tolower(static_cast<unsigned char>(a))==std::tolower(static_cast<unsigned char>(b));})!=bytes.end();
    };
    return contains(narrow)||contains(wideName);
  }
};

// hostAction 285 (Host 제거 정리), one record per real removal attempt. Nothing
// is written for a run that was not a removal.
inline void journal(const Result& result){
  const bool previous=operationalLogEnabled;operationalLogEnabled=true;
  const std::string outcome=result.outcome;
  recordHostEvent("hostAction",{{"action",285},{"stage",1},{"ok",outcome=="cleaned"},{"error",reasonNumber(result.reason)},
    {"items",result.nodesRemoved},{"missing",result.nodesGone},{"pending",result.nodesPending+result.nodesUnknown>0},
    {"dropped",result.packagesRemoved},{"expected",result.packagesKept+result.packagesFailed},
    {"changed",result.certificateEntries>0},{"cause",certificateNumber(result.certificate)},{"owned",std::string(result.state)=="removed"},
    {"fallback",result.foreignLock}});
  operationalLogEnabled=previous;
}
inline int command(int argc,wchar_t** argv){
  std::vector<std::wstring> arguments;for(int index=2;index<argc;++index)arguments.emplace_back(argv[index]);
  const auto request=parseRequest(arguments,installedProductCode());
  Result result;
  if(request.removal){
    requireAdmin();
    Machine machine;result=cleanup(machine,request);
    if(std::string(result.outcome)!="skipped")journal(result);
  }else result.reason=request.refusal;  // not a removal: the machine is not looked at
  std::cout<<resultJson(result)<<'\n';
  const std::string outcome=result.outcome;
  return outcome=="cleaned"||outcome=="skipped"?0:20;
}
}  // namespace host::removal
