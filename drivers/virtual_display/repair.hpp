#pragma once
#include <functional>
#include <shlobj.h>

// Repair deliberately targets only the shared Root\MttVDD hardware identity.
// Driver-store packages, configuration directories/files and other adapters
// are never removal targets. A plan is an observation, not authorization.
namespace repair {
struct Value {
  bool present{};DWORD type{};std::vector<BYTE> bytes;
  bool operator==(const Value&)const=default;
};
struct Device {
  std::wstring id,name;std::vector<std::wstring> hardware;
  std::wstring driverIdentity;bool ready{},restartRequired{};
  bool operator==(const Device&)const=default;
};
struct Snapshot {
  std::vector<Device> devices;std::array<Value,3> values;
  std::wstring packagePath;
};
constexpr const wchar_t* paths[]={kVendor,kOwner,kOwner};
constexpr const wchar_t* names[]={L"VDDPATH",kMarker,L"PackagePath"};
static std::wstring valueText(const Value& value) {
  if(!value.present)return {};
  require(value.type==REG_SZ&&value.bytes.size()>=sizeof(wchar_t)&&value.bytes.size()%sizeof(wchar_t)==0&&value.bytes.size()<=8192,"repair_registry_type_unsupported");
  std::wstring text(value.bytes.size()/sizeof(wchar_t),L'\0');std::memcpy(text.data(),value.bytes.data(),value.bytes.size());
  require(text.back()==L'\0'&&text.find(L'\0')==text.size()-1,"repair_registry_value_invalid");text.pop_back();return text;
}
static bool target(const Device& device) {
  return device.hardware.size()==1&&!_wcsicmp(device.hardware[0].c_str(),kHardware);
}
static std::string digest(const std::string& bytes) {
  BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::array<BYTE,32> out{};
  bool ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
  if(ok)ok=bytes.size()<=1048576&&BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),ULONG(bytes.size()),0)>=0&&BCryptFinishHash(hash,out.data(),ULONG(out.size()),0)>=0;
  if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);require(ok,"repair_hash_failed");
  constexpr char hex[]="0123456789abcdef";std::string result;for(const auto byte:out){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
static std::string hex(const std::vector<BYTE>& bytes) {constexpr char digits[]="0123456789abcdef";std::string result;for(const auto byte:bytes){result+=digits[byte>>4];result+=digits[byte&15];}return result;}
static std::string serialized(const Snapshot& snapshot) {
  std::ostringstream out;out<<"{\"format\":\"multitap-display-repair-v1\",\"packagePath\":"<<json(snapshot.packagePath)<<",\"devices\":[";
  bool first=true;for(const auto& device:snapshot.devices){if(!first)out<<',';first=false;out<<"{\"instanceId\":"<<json(device.id)<<",\"name\":"<<json(device.name)<<",\"driverIdentity\":"<<json(device.driverIdentity)<<",\"ready\":"<<(device.ready?"true":"false")<<",\"restartRequired\":"<<(device.restartRequired?"true":"false")<<",\"hardware\":[";bool firstHardware=true;for(const auto& hardware:device.hardware){if(!firstHardware)out<<',';firstHardware=false;out<<json(hardware);}out<<"]}";}
  out<<"],\"configuration\":[";for(size_t i=0;i<snapshot.values.size();++i){if(i)out<<',';const auto& value=snapshot.values[i];out<<"{\"present\":"<<(value.present?"true":"false")<<",\"type\":"<<value.type<<",\"bytes\":\""<<hex(value.bytes)<<"\"}";}out<<"]}";return out.str();
}
struct Plan {Snapshot snapshot;std::string id;std::vector<Device> devices;std::vector<size_t> values;};
static Plan plan(Snapshot snapshot) {
  require(snapshot.devices.size()<=64,"repair_device_limit");
  std::sort(snapshot.devices.begin(),snapshot.devices.end(),[](const Device&a,const Device&b){return a.id<b.id;});
  for(size_t i=0;i<snapshot.devices.size();++i){require(!snapshot.devices[i].id.empty()&&snapshot.devices[i].id.size()<=4096,"repair_device_identity_invalid");if(i)require(snapshot.devices[i-1].id!=snapshot.devices[i].id,"repair_duplicate_device");require(target(snapshot.devices[i])||std::none_of(snapshot.devices[i].hardware.begin(),snapshot.devices[i].hardware.end(),[](const std::wstring& id){return !_wcsicmp(id.c_str(),kHardware);}),"repair_ambiguous_hardware_id");}
  const auto vendor=valueText(snapshot.values[0]),marker=valueText(snapshot.values[1]),ownedPath=valueText(snapshot.values[2]);
  Plan result;result.snapshot=std::move(snapshot);result.id=digest(serialized(result.snapshot));
  for(const auto& device:result.snapshot.devices)if(target(device))result.devices.push_back(device);
  const bool healthy=result.devices.size()==1&&result.devices[0].ready&&!_wcsicmp(result.devices[0].id.c_str(),marker.c_str())&&!vendor.empty()&&samePath(vendor,ownedPath);
  if(healthy){result.devices.clear();return result;}
  const bool needsRepair=!result.devices.empty()||!vendor.empty()||!marker.empty()||!ownedPath.empty();
  if(needsRepair)for(size_t i=0;i<result.snapshot.values.size();++i)if(result.snapshot.values[i].present)result.values.push_back(i);
  return result;
}
static std::string safeReason(const char* reason) {const std::string value=reason?reason:"";return !value.empty()&&value.size()<=80&&value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos?value:"display_repair_failed";}
static std::string planJson(const Plan& value,bool trustedPackage,const std::string& reason) {
  std::ostringstream out;out<<"{\"planId\":\""<<value.id<<"\",\"canRepair\":"<<(trustedPackage?"true":"false")<<",\"repairNeeded\":"<<(!value.devices.empty()||!value.values.empty()?"true":"false")<<",\"reason\":\""<<reason<<"\",\"deviceCount\":"<<value.devices.size()<<",\"configurationCount\":"<<value.values.size()<<",\"driverStoreRetained\":true,\"configurationFilesRetained\":true,\"items\":[";
  bool first=true;size_t index=0;for(const auto& device:value.devices){if(!first)out<<',';first=false;out<<"{\"id\":\"device-"<<index++<<"\",\"kind\":\"device\",\"name\":"<<json(device.name.empty()?L"Shared virtual display adapter":device.name)<<",\"instanceId\":"<<json(device.id)<<",\"hardwareId\":"<<json(kHardware)<<",\"impact\":\"Remove this shared virtual display device; its virtual screens will disconnect.\",\"required\":true}";}
  for(const auto item:value.values){if(!first)out<<',';first=false;out<<"{\"id\":\"configuration-"<<item<<"\",\"kind\":\"configuration\",\"name\":"<<json(names[item])<<",\"registryPath\":"<<json(paths[item])<<",\"registryValue\":"<<json(names[item])<<",\"impact\":\"Back up and clear this remaining shared virtual display setting; keep configuration files.\",\"required\":true}";}
  out<<"]}";return out.str();
}
struct Result {
  bool installed{},cleanupCompleted{},configurationRestored=true,configurationRestoreAttempted{},partialRemoval{},removalUncertain{},rebootRequired{},configurationPending{};
  size_t removed{},removeRequested{},cleared{};std::string backupId,reason;std::string state="null";
};
struct Access {
  std::function<bool()> administrator;
  std::function<void()> verifyTrusted;
  std::function<Snapshot()> observe;
  std::function<std::string(const Snapshot&)> backup;
  std::function<void(const Device&)> remove;
  std::function<void(size_t,const Value&)> clear;
  std::function<bool(const Snapshot&,const std::vector<size_t>&)> restore;
  std::function<void(Result&)> installTrusted;
};
static Result execute(const std::string& planId,const Access& access) {
  Result result;Snapshot original;std::vector<size_t> touched;std::vector<Device> selected;bool attemptedRemoval=false,attemptedInstall=false;
  try {
    require(access.administrator(),"administrator_required");
    require(planId.size()==64&&planId.find_first_not_of("0123456789abcdef")==std::string::npos,"invalid_repair_plan");
    access.verifyTrusted();const auto approved=plan(access.observe());original=approved.snapshot;
    require(approved.id==planId,"repair_plan_changed");require(!approved.devices.empty()||!approved.values.empty(),"display_repair_not_required");selected=approved.devices;
    result.backupId=access.backup(original);require(!result.backupId.empty(),"repair_backup_failed");
    auto expected=original;
    const auto unchanged=[&]{require(digest(serialized(plan(access.observe()).snapshot))==digest(serialized(expected)),"repair_state_changed");};
    for(const auto& device:approved.devices){unchanged();attemptedRemoval=true;access.remove(device);++result.removeRequested;std::erase_if(expected.devices,[&](const Device& item){return item.id==device.id;});unchanged();++result.removed;}
    for(const auto item:approved.values){unchanged();touched.push_back(item);access.clear(item,original.values[item]);++result.cleared;expected.values[item]={};unchanged();}
    unchanged();result.cleanupCompleted=true;attemptedInstall=true;access.installTrusted(result);require(result.installed,"display_repair_install_incomplete");
  } catch(const std::exception& error) {
    result.installed=false;result.reason=safeReason(error.what());
    if(attemptedRemoval){try{const auto actual=access.observe();result.removed=size_t(std::count_if(selected.begin(),selected.end(),[&](const Device& device){return std::none_of(actual.devices.begin(),actual.devices.end(),[&](const Device& current){return current.id==device.id;});}));result.removalUncertain=result.removed!=result.removeRequested;}catch(...){result.removalUncertain=true;}}
    result.partialRemoval=result.removed>0;
    if(attemptedInstall)touched={0,1,2};
    if(!touched.empty()){result.configurationRestoreAttempted=true;try{result.configurationRestored=access.restore(original,touched);}catch(...){result.configurationRestored=false;}result.cleanupCompleted=false;}
  }
  return result;
}
static std::string resultJson(const Result& value) {
  std::ostringstream out;out<<std::boolalpha<<"{\"repairInstalled\":"<<value.installed<<",\"cleanupCompleted\":"<<value.cleanupCompleted<<",\"removedDeviceCount\":"<<value.removed<<",\"removeRequestedCount\":"<<value.removeRequested<<",\"removalUncertain\":"<<value.removalUncertain<<",\"clearedConfigurationCount\":"<<value.cleared<<",\"configurationRestored\":"<<value.configurationRestored<<",\"configurationRestoreAttempted\":"<<value.configurationRestoreAttempted<<",\"partialRemoval\":"<<value.partialRemoval<<",\"backupId\":\""<<value.backupId<<"\",\"rebootRequired\":"<<value.rebootRequired<<",\"configurationPending\":"<<value.configurationPending<<",\"driverStoreRetained\":true,\"configurationFilesRetained\":true,\"reason\":\""<<value.reason<<"\",\"state\":"<<value.state<<'}';return out.str();
}
static Value readValue(size_t index) {
  Reg key;const auto opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,paths[index],0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key.h);
  if(opened==ERROR_FILE_NOT_FOUND||opened==ERROR_PATH_NOT_FOUND)return {};
  require(opened==ERROR_SUCCESS,"repair_registry_unavailable");DWORD type=0,count=0;auto error=RegQueryValueExW(key.h,names[index],nullptr,&type,nullptr,&count);
  if(error==ERROR_FILE_NOT_FOUND)return {};
  require(error==ERROR_SUCCESS&&count<=8192,"repair_registry_unavailable");Value result{true,type,std::vector<BYTE>(count)};
  error=RegQueryValueExW(key.h,names[index],nullptr,&result.type,result.bytes.data(),&count);require(error==ERROR_SUCCESS&&count==result.bytes.size(),"repair_registry_changed");valueText(result);return result;
}
static std::vector<std::wstring> hardwareIds(HDEVINFO set,SP_DEVINFO_DATA& device) {
  DWORD type=0,count=0;const bool sized=SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_HARDWAREID,&type,nullptr,0,&count)!=FALSE;
  const auto error=sized?ERROR_SUCCESS:GetLastError();if(error==ERROR_INVALID_DATA)return {};
  require(error==ERROR_INSUFFICIENT_BUFFER&&count>=sizeof(wchar_t)*2&&count<=16384&&count%sizeof(wchar_t)==0,"repair_device_hardware_unavailable");
  std::vector<wchar_t> value(count/sizeof(wchar_t)+2,L'\0');require(SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_HARDWAREID,&type,reinterpret_cast<PBYTE>(value.data()),DWORD(value.size()*sizeof(wchar_t)),&count)&&type==REG_MULTI_SZ,"repair_device_hardware_unavailable");
  std::vector<std::wstring> result;for(size_t at=0;at<value.size()&&value[at];){const auto length=wcsnlen_s(value.data()+at,value.size()-at);require(length<value.size()-at&&result.size()<32,"repair_device_hardware_unavailable");result.emplace_back(value.data()+at,length);at+=length+1;}return result;
}
static Snapshot observe() {
  Snapshot snapshot;snapshot.packagePath=package().lexically_normal().wstring();for(size_t i=0;i<snapshot.values.size();++i)snapshot.values[i]=readValue(i);
  Devices set{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,nullptr,nullptr,0)};require(set.h!=INVALID_HANDLE_VALUE,"repair_device_enumeration_failed");
  for(DWORD i=0;;++i){SP_DEVINFO_DATA device{sizeof(device)};if(!SetupDiEnumDeviceInfo(set.h,i,&device)){require(GetLastError()==ERROR_NO_MORE_ITEMS,"repair_device_enumeration_failed");break;}require(snapshot.devices.size()<64,"repair_device_limit");Device item;item.id=instance(set.h,device);item.hardware=hardwareIds(set.h,device);item.name=deviceText(set.h,device,SPDRP_FRIENDLYNAME);if(item.name.empty())item.name=deviceText(set.h,device,SPDRP_DEVICEDESC);
    ULONG flags=0,problem=0;const auto state=CM_Get_DevNode_Status(&flags,&problem,device.DevInst,0);item.ready=state==CR_SUCCESS&&(flags&DN_STARTED)&&!(flags&DN_HAS_PROBLEM);item.restartRequired=state==CR_SUCCESS&&((flags&DN_NEED_RESTART)||problem==CM_PROB_NEED_RESTART);
    item.driverIdentity=deviceText(set.h,device,SPDRP_DRIVER);Reg driver{SetupDiOpenDevRegKey(set.h,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE)};if(driver.h!=INVALID_HANDLE_VALUE){for(const auto name:{L"InfPath",L"DriverVersion",L"ProviderName"}){wchar_t value[1024]{};DWORD size=sizeof(value);if(RegGetValueW(driver.h,nullptr,name,RRF_RT_REG_SZ,nullptr,value,&size)==ERROR_SUCCESS)item.driverIdentity+=L"|"+std::wstring(value);}}else driver.h=nullptr;
    snapshot.devices.push_back(std::move(item));}
  return plan(std::move(snapshot)).snapshot;
}
static void removeExact(const Device& expected) {
  require(target(expected),"repair_device_not_allowed");Devices set{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,nullptr,nullptr,0)};require(set.h!=INVALID_HANDLE_VALUE,"repair_device_enumeration_failed");
  SP_DEVINFO_DATA device{sizeof(device)};require(SetupDiOpenDeviceInfoW(set.h,expected.id.c_str(),nullptr,0,&device),"repair_device_changed");
  Device fresh;fresh.id=instance(set.h,device);fresh.hardware=hardwareIds(set.h,device);
  require(fresh.id==expected.id&&target(fresh)&&fresh.hardware==expected.hardware,"repair_device_changed");removeDevice(set.h,device);
}
static void clearValue(size_t index,const Value& expected) {
  require(index<3&&readValue(index)==expected,"repair_registry_changed");Reg key;
  require(RegOpenKeyExW(HKEY_LOCAL_MACHINE,paths[index],0,KEY_QUERY_VALUE|KEY_SET_VALUE|KEY_WOW64_64KEY,&key.h)==ERROR_SUCCESS,"repair_registry_write_denied");
  require(readValue(index)==expected&&RegDeleteValueW(key.h,names[index])==ERROR_SUCCESS,"repair_registry_changed");
}
static bool restoreValues(const Snapshot& original,const std::vector<size_t>& touched) {
  const auto current=observe();for(const auto& device:current.devices)if(target(device)&&std::none_of(original.devices.begin(),original.devices.end(),[&](const Device& item){return item==device;}))return false;
  for(const auto index:touched)if(current.values[index].present&&current.values[index]!=original.values[index])return false;
  bool restored=true;for(const auto index:touched){try{const auto latest=readValue(index);if(latest==original.values[index])continue;require(!latest.present,"repair_registry_changed");const auto fresh=observe();for(const auto& device:fresh.devices)require(!target(device)||std::any_of(original.devices.begin(),original.devices.end(),[&](const Device& prior){return prior==device;}),"repair_state_changed");Reg key;require(RegCreateKeyExW(HKEY_LOCAL_MACHINE,paths[index],0,nullptr,0,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&key.h,nullptr)==ERROR_SUCCESS,"repair_registry_write_denied");const auto& value=original.values[index];require(!readValue(index).present&&RegSetValueExW(key.h,names[index],0,value.type,value.bytes.data(),DWORD(value.bytes.size()))==ERROR_SUCCESS,"repair_registry_restore_failed");}catch(...){restored=false;}}return restored;
}
static fs::path backupDirectory() {
  PWSTR known=nullptr;require(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&known)==S_OK,"repair_backup_path_unavailable");const fs::path base(known);CoTaskMemFree(known);require(base.is_absolute(),"repair_backup_path_unavailable");
  const auto parent=base/L"Multitap",path=parent/L"DisplayRepair";PSECURITY_DESCRIPTOR descriptor=nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr),"repair_backup_acl_failed");SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
  for(const auto& directory:{parent,path}){const bool ok=CreateDirectoryW(directory.c_str(),&security)!=FALSE||GetLastError()==ERROR_ALREADY_EXISTS;if(!ok){LocalFree(descriptor);throw std::runtime_error("repair_backup_directory_failed");}}
  LocalFree(descriptor);viewport::DirectoryLock parentLock(parent);viewport::DirectoryLock lock(path);return path;
}
static std::string backup(const Snapshot& snapshot) {
  const auto directory=backupDirectory();viewport::DirectoryLock parentLock(directory.parent_path());viewport::DirectoryLock lock(directory);std::array<BYTE,16> random{};require(BCryptGenRandom(nullptr,random.data(),ULONG(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"repair_random_failed");const std::string id=hex(std::vector<BYTE>(random.begin(),random.end()));
  const auto path=directory/(L"repair-"+std::wstring(id.begin(),id.end())+L".json");const auto bytes=serialized(snapshot),hash=digest(bytes);viewport::writeNew(path,bytes);require(viewport::readFile(path,1048576)==bytes&&sha256(path)==hash,"repair_backup_verification_failed");return id;
}
static bool trustedDescriptor(PSECURITY_DESCRIPTOR descriptor) {
  PSID owner=nullptr;PACL dacl=nullptr;BOOL ownerDefault=FALSE,present=FALSE,defaulted=FALSE;
  if(!GetSecurityDescriptorOwner(descriptor,&owner,&ownerDefault)||!owner||!viewport::privilegedSid(owner)||!GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted)||!present||!dacl)return false;
  for(DWORD i=0;i<dacl->AceCount;++i){void* raw=nullptr;if(!GetAce(dacl,i,&raw))return false;const auto* header=static_cast<ACE_HEADER*>(raw);if(header->AceFlags&INHERIT_ONLY_ACE)continue;if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);if(!viewport::privilegedSid(const_cast<DWORD*>(&ace->SidStart)))return false;}else if(header->AceType!=ACCESS_DENIED_ACE_TYPE)return false;}return true;
}
struct Mutex {
  viewport::Handle handle;bool acquired{};
  Mutex(){PSECURITY_DESCRIPTOR descriptor=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr),"repair_mutex_acl_failed");SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};handle.h=CreateMutexW(&security,FALSE,L"Global\\Multitap.Display.Viewport");LocalFree(descriptor);require(handle.h&&handle.h!=INVALID_HANDLE_VALUE,"repair_busy");PSECURITY_DESCRIPTOR actual=nullptr;const auto queried=GetSecurityInfo(handle.h,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&actual);const bool safe=queried==ERROR_SUCCESS&&trustedDescriptor(actual);if(actual)LocalFree(actual);require(safe,"repair_mutex_untrusted");const auto waited=WaitForSingleObject(handle.h,0);acquired=waited==WAIT_OBJECT_0||waited==WAIT_ABANDONED;require(acquired,"repair_busy");}
  ~Mutex(){if(acquired)ReleaseMutex(handle.h);}
};
static void outputPlan() {
  const auto value=plan(observe());bool trustedPackage=true;std::string reason;
  try{PackageLock lock(package());verify();}catch(const std::exception& error){trustedPackage=false;reason=safeReason(error.what());}
  std::cout<<planJson(value,trustedPackage,reason)<<'\n';
}
static int outputInstall(const std::wstring& requested) {
  Result result;
  try {
    require(elevated(),"administrator_required");Mutex mutex;PackageLock packageLock(package());
    Access access{elevated,verify,observe,backup,removeExact,clearValue,restoreValues,[](Result& value){
      std::ostringstream hidden;auto* old=std::cout.rdbuf(hidden.rdbuf());try{install();}catch(...){std::cout.rdbuf(old);throw;}std::cout.rdbuf(old);
      const auto after=status();value.installed=after.installed&&after.owned&&after.deviceCount==1&&after.foreignDeviceCount==0;value.state=statusJson(after);value.rebootRequired=after.restartRequired||hidden.str().find("\"rebootRequired\":true")!=std::string::npos;value.configurationPending=!after.ready||after.displays.empty()||!consoleDesktop();
    }};
    result=execute(utf8(requested),access);
    if(!result.installed){try{const auto after=status();result.state=statusJson(after);result.rebootRequired=after.restartRequired;result.configurationPending=after.owned&&*statusReason(after,consoleDesktop());}catch(...){}}
  }catch(const std::exception& error){result.reason=safeReason(error.what());}
  std::cout<<resultJson(result)<<'\n';return result.installed?0:1;
}
}
