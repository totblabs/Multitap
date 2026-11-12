#pragma once
// Native Apps & Features registration and removal of the Windows client. No
// script executes deletions, and no user profile, credential, dependency or
// trust store is changed here.
#include "setup_payload.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>

namespace multitap::installation {
using namespace payload;
// The Apps & Features entry's MultitapProduct value and the removal stage's prefix.
constexpr const wchar_t* ProductId=L"client";
constexpr const wchar_t* MarkerName=L"multitap-client.json";
constexpr const char* MarkerProduct="Multitap";
inline std::wstring rootPath(const Roots& roots){return roots.programFiles+L"\\Multitap\\Client";}
// A Client\<x.y.z> folder of an earlier layout.
inline std::wstring versionExecutable(const Roots& roots,const std::string& version){return rootPath(roots)+L"\\"+std::wstring(version.begin(),version.end())+L"\\Multitap.exe";}
// The client lives in one fixed folder: Client\Multitap.exe with its marker beside it.
inline std::wstring clientExecutable(const Roots& roots){return rootPath(roots)+L"\\Multitap.exe";}
inline std::wstring arpPath(){return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MultitapClient";}
inline void ensure(bool ok,const char* code){if(!ok)throw Failure{code};}
struct Key {
  HKEY value=nullptr;
  Key()=default;Key(const Key&)=delete;Key& operator=(const Key&)=delete;
  ~Key(){if(value)RegCloseKey(value);}
};
inline std::optional<std::wstring> registryText(HKEY key,const wchar_t* name){
  DWORD type=0,size=0;const auto first=RegQueryValueExW(key,name,nullptr,&type,nullptr,&size);
  if(first==ERROR_FILE_NOT_FOUND)return std::nullopt;
  ensure(first==ERROR_SUCCESS&&type==REG_SZ&&size>=sizeof(wchar_t)&&size<=65536&&size%sizeof(wchar_t)==0,"uninstall_registry_invalid");
  std::wstring text(size/sizeof(wchar_t),L'\0');
  ensure(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(text.data()),&size)==ERROR_SUCCESS&&text.back()==L'\0',"uninstall_registry_invalid");
  text.pop_back();ensure(text.find(L'\0')==std::wstring::npos,"uninstall_registry_invalid");return text;
}
inline void registryText(HKEY key,const wchar_t* name,const std::wstring& text){ensure(RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),DWORD((text.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"uninstall_registry_write_failed");}
inline void registryNumber(HKEY key,const wchar_t* name,DWORD value){ensure(RegSetValueExW(key,name,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS,"uninstall_registry_write_failed");}
struct Registration {HKEY hive=HKEY_LOCAL_MACHINE;std::wstring key;bool qa=false;};
struct RegistryValue {std::wstring name;DWORD type=0;std::vector<BYTE> bytes;};
inline std::vector<RegistryValue> registrySnapshot(HKEY key){
  DWORD children=0,count=0;ensure(RegQueryInfoKeyW(key,nullptr,nullptr,nullptr,&children,nullptr,nullptr,&count,nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS&&children==0&&count<=64,"uninstall_registry_invalid");
  std::vector<RegistryValue> out;
  for(DWORD i=0;i<count;++i){RegistryValue value;std::wstring name(16384,L'\0');DWORD length=DWORD(name.size()),size=65536;value.bytes.resize(size);ensure(RegEnumValueW(key,i,name.data(),&length,nullptr,&value.type,value.bytes.data(),&size)==ERROR_SUCCESS,"uninstall_registry_invalid");name.resize(length);value.name=std::move(name);value.bytes.resize(size);out.push_back(std::move(value));}
  return out;
}
inline Registration registration(){return {HKEY_LOCAL_MACHINE,arpPath(),false};}
inline bool registrationOwned(HKEY key,const Roots& roots){
  const auto owner=registryText(key,L"MultitapProduct"),location=registryText(key,L"InstallLocation"),command=registryText(key,L"UninstallString");
  return owner&&*owner==ProductId&&location&&sameText(*location,rootPath(roots))&&command&&sameText(*command,L"\""+rootPath(roots)+L"\\Uninstall.exe\" --uninstall");
}
inline void checkRegistration(const Registration& reg,const Roots& roots,bool required){
  Key key;const auto result=RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ|KEY_WOW64_64KEY,&key.value);
  if(result==ERROR_FILE_NOT_FOUND&&!required)return;
  ensure(result==ERROR_SUCCESS&&registrationOwned(key.value,roots),"uninstall_foreign_registration");
}
inline Handle verifiedFile(const std::wstring& path,bool protectedFiles,DWORD access=GENERIC_READ){
  ensure(canonicalPath(path),"uninstall_path_invalid");
  Handle file(CreateFileW(path.c_str(),access|READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  ensure(bool(file)&&!(attributesOf(file.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"uninstall_file_unsafe");
  BY_HANDLE_FILE_INFORMATION info{};
  ensure(GetFileInformationByHandle(file.get(),&info)&&info.nNumberOfLinks==1&&sameText(finalPath(file.get()),expectedFinal(path)),"uninstall_file_unsafe");
  if(protectedFiles)protectedDirectory(file.get());return file;
}
inline std::vector<char> readFile(HANDLE file,size_t maximum=64<<20){
  LARGE_INTEGER size{};ensure(GetFileSizeEx(file,&size)&&size.QuadPart>=0&&static_cast<unsigned long long>(size.QuadPart)<=maximum,"uninstall_file_size_invalid");
  std::vector<char> out(size_t(size.QuadPart));DWORD read=0;
  ensure(out.empty()||(ReadFile(file,out.data(),DWORD(out.size()),&read,nullptr)&&read==out.size()),"uninstall_file_read_failed");return out;
}
inline Digest fileDigest(const std::wstring& path,bool protectedFiles){auto file=verifiedFile(path,protectedFiles);auto bytes=readFile(file.get());Sha256 hash;hash.add(bytes.data(),DWORD(bytes.size()));return hash.finish();}
inline std::wstring modulePath(){std::wstring path(32768,L'\0');const auto size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));ensure(size&&size<path.size(),"uninstall_module_invalid");path.resize(size);return path;}
inline void verifyMappedModule(HANDLE file){
  std::wstring mapped(32768,L'\0'),opened(32768,L'\0');const auto mapSize=K32GetMappedFileNameW(GetCurrentProcess(),GetModuleHandleW(nullptr),mapped.data(),DWORD(mapped.size()));const auto openSize=GetFinalPathNameByHandleW(file,opened.data(),DWORD(opened.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_NT);
  ensure(mapSize&&mapSize<mapped.size()&&openSize&&openSize<opened.size(),"uninstall_running_image_unverified");mapped.resize(mapSize);opened.resize(openSize);ensure(sameText(mapped,opened),"uninstall_running_image_replaced");
}
inline void verifyRoot(const Roots& roots){
  const auto root=rootPath(roots);ensure(canonicalPath(roots.programFiles)&&canonicalPath(root),"uninstall_path_invalid");plainChain(root,{});
  auto directory=openDirectory(root,READ_CONTROL);ensure(sameText(finalPath(directory.get()),expectedFinal(root)),"uninstall_root_redirected");if(!roots.qa)protectedChain(root,roots.programFiles);
}
inline bool ownedVersion(const Roots& roots,const std::wstring& name){
  try{
    if(!versionName(name))return false;
    const auto directory=rootPath(roots)+L"\\"+name;plainChain(directory,roots.programFiles);
    auto folder=openDirectory(directory,READ_CONTROL);if(!roots.qa)protectedDirectory(folder.get());
    auto marker=verifiedFile(directory+L"\\"+MarkerName,!roots.qa);const auto bytes=readFile(marker.get(),4096);
    if(!clientOwnershipText(std::string(bytes.begin(),bytes.end()),MarkerProduct,utf8(name)))return false;
    auto executable=verifiedFile(versionExecutable(roots,utf8(name)),!roots.qa,FILE_READ_ATTRIBUTES);return bool(executable);
  }catch(...){return false;}
}
// The fixed client installation of [version]: root marker naming it and a plain Multitap.exe.
inline bool fixedClientOwned(const Roots& roots,const std::string& version){
  try{
    const auto root=rootPath(roots);verifyRoot(roots);
    auto marker=verifiedFile(root+L"\\"+MarkerName,!roots.qa);const auto bytes=readFile(marker.get(),4096);
    if(!clientOwnershipText(std::string(bytes.begin(),bytes.end()),MarkerProduct,version))return false;
    auto executable=verifiedFile(clientExecutable(roots),!roots.qa,FILE_READ_ATTRIBUTES);return bool(executable);
  }catch(...){return false;}
}
inline std::vector<std::wstring> ownedVersions(const Roots& roots){
  verifyRoot(roots);auto directory=openDirectory(rootPath(roots),READ_CONTROL|FILE_LIST_DIRECTORY);std::vector<std::wstring> out;
  for(const auto& child:listByHandle(directory.get()))if((child.attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(child.attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&ownedVersion(roots,child.name))out.push_back(child.name);
  std::sort(out.begin(),out.end(),NoCase{});return out;
}
class InstallationGate {
 public:
  InstallationGate(){mutex_=Handle(CreateMutexW(nullptr,FALSE,L"Global\\MultitapClient.Installer"));ensure(bool(mutex_),"uninstall_installer_gate_unavailable");const auto result=WaitForSingleObject(mutex_.get(),0);ensure(result==WAIT_OBJECT_0||result==WAIT_ABANDONED,"uninstall_installation_in_progress");held_=true;}
  ~InstallationGate(){if(held_)ReleaseMutex(mutex_.get());}
  InstallationGate(const InstallationGate&)=delete;InstallationGate& operator=(const InstallationGate&)=delete;
 private:Handle mutex_;bool held_=false;
};
inline void registerInstalled(const Roots& roots,const std::string& version,const std::wstring& setup,const Registration& reg,const std::function<void()>& beforeRegistryWrite={}){
  ensure(validVersion(version)&&fixedClientOwned(roots,version),"uninstall_installation_unverified");verifyRoot(roots);checkRegistration(reg,roots,false);
  const auto uninstaller=rootPath(roots)+L"\\Uninstall.exe";
  std::optional<std::vector<char>> previous;
  if(GetFileAttributesW(uninstaller.c_str())!=INVALID_FILE_ATTRIBUTES){checkRegistration(reg,roots,true);auto held=verifiedFile(uninstaller,!roots.qa);previous=readFile(held.get());}
  bool registryExisted=false;std::vector<RegistryValue> oldValues;
  {Key old;const auto code=RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ|KEY_WOW64_64KEY,&old.value);if(code==ERROR_SUCCESS){registryExisted=true;oldValues=registrySnapshot(old.value);}else ensure(code==ERROR_FILE_NOT_FOUND,"uninstall_registry_invalid");}
  auto source=verifiedFile(setup,false);if(!roots.qa){ensure(sameText(setup,modulePath()),"uninstall_running_setup_required");verifyMappedModule(source.get());}const auto bytes=readFile(source.get());ensure(bytes.size()>2&&bytes[0]=='M'&&bytes[1]=='Z',"uninstall_setup_invalid");
  bool placed=false;
  try{
  replaceFile(uninstaller,bytes);placed=true;ensure(fileDigest(uninstaller,!roots.qa)==fileDigest(setup,false),"uninstall_copy_verify_failed");
  Key key;DWORD disposition=0;
  ensure(RegCreateKeyExW(reg.hive,reg.key.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,nullptr,&key.value,&disposition)==ERROR_SUCCESS,"uninstall_registry_write_failed");
  if(disposition==REG_OPENED_EXISTING_KEY)ensure(registrationOwned(key.value,roots),"uninstall_foreign_registration");
  if(beforeRegistryWrite)beforeRegistryWrite();
  registryText(key.value,L"MultitapProduct",ProductId);registryText(key.value,L"InstallLocation",rootPath(roots));registryText(key.value,L"UninstallString",L"\""+uninstaller+L"\" --uninstall");
  registryText(key.value,L"DisplayName",L"Multitap");registryText(key.value,L"DisplayVersion",std::wstring(version.begin(),version.end()));registryText(key.value,L"Publisher",L"Multitap");
  registryText(key.value,L"DisplayIcon",clientExecutable(roots)+L",0");registryNumber(key.value,L"NoModify",1);registryNumber(key.value,L"NoRepair",1);
  ensure(RegFlushKey(key.value)==ERROR_SUCCESS,"uninstall_registry_write_failed");
  }catch(...){
    if(placed){if(previous)replaceFile(uninstaller,*previous);else{auto file=verifiedFile(uninstaller,!roots.qa,DELETE);ensure(deleteByHandle(file.get()),"uninstall_registration_rollback_failed");}}
    if(registryExisted){Key old;ensure(RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,&old.value)==ERROR_SUCCESS,"uninstall_registration_rollback_failed");for(const auto& value:registrySnapshot(old.value))ensure(RegDeleteValueW(old.value,value.name.c_str())==ERROR_SUCCESS,"uninstall_registration_rollback_failed");for(const auto& value:oldValues)ensure(RegSetValueExW(old.value,value.name.c_str(),0,value.type,value.bytes.data(),DWORD(value.bytes.size()))==ERROR_SUCCESS,"uninstall_registration_rollback_failed");}
    else{const auto removed=RegDeleteKeyExW(reg.hive,reg.key.c_str(),KEY_WOW64_64KEY,0);ensure(removed==ERROR_SUCCESS||removed==ERROR_FILE_NOT_FOUND,"uninstall_registration_rollback_failed");}
    throw;
  }
}
inline void verifyTree(const std::wstring& path,bool protectedFiles,unsigned depth=0,bool requireDelete=false){
  ensure(depth<=48,"uninstall_tree_too_deep");auto folder=openDirectory(path,READ_CONTROL|FILE_LIST_DIRECTORY);
  ensure(sameText(finalPath(folder.get()),expectedFinal(path)),"uninstall_root_redirected");if(protectedFiles)protectedDirectory(folder.get());
  for(const auto& entry:listByHandle(folder.get())){
    ensure(!(entry.attributes&FILE_ATTRIBUTE_REPARSE_POINT),"uninstall_tree_link_refused");const auto child=path+L"\\"+entry.name;
    if(entry.attributes&FILE_ATTRIBUTE_DIRECTORY)verifyTree(child,protectedFiles,depth+1,requireDelete);else{auto file=verifiedFile(child,protectedFiles,FILE_READ_ATTRIBUTES|(requireDelete?DELETE:0));(void)file;}
  }
}
struct Plan {std::vector<std::wstring> versions;std::string version;Digest uninstaller{};};
inline Plan preflight(const Roots& roots,const Registration& reg,const std::string& compiledVersion){
  ensure(validVersion(compiledVersion),"uninstall_version_invalid");checkRegistration(reg,roots,true);verifyRoot(roots);
  Key key;ensure(RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ|KEY_WOW64_64KEY,&key.value)==ERROR_SUCCESS,"uninstall_registry_invalid");const auto version=registryText(key.value,L"DisplayVersion");
  ensure(version&&*version==std::wstring(compiledVersion.begin(),compiledVersion.end())&&fixedClientOwned(roots,compiledVersion),"uninstall_version_mismatch");
  // The fixed folder plus any Client\<x.y.z> left by an earlier layout.
  Plan out;out.version=compiledVersion;out.versions=ownedVersions(roots);out.uninstaller=fileDigest(rootPath(roots)+L"\\Uninstall.exe",!roots.qa);
  verifyTree(rootPath(roots),!roots.qa);
  return out;
}
inline void removeShortcut(const std::wstring& path,const std::function<bool(const std::wstring&,const std::wstring&)>& owned){
  if(!canonicalPath(path))return;
  try{
    const auto bytes=shortcutBytes(path);if(!bytes)return;
    Apartment apartment;Com<IShellLinkW> link;ensure(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))),"uninstall_shortcut_unavailable");loadShortcut(link.get(),*bytes);
    std::wstring target(32768,L'\0'),arguments(32768,L'\0');ensure(SUCCEEDED(link->GetPath(target.data(),int(target.size()),nullptr,0))&&SUCCEEDED(link->GetArguments(arguments.data(),int(arguments.size()))),"uninstall_shortcut_unreadable");
    target.resize(std::wcslen(target.c_str()));arguments.resize(std::wcslen(arguments.c_str()));if(!owned(target,arguments))return;
    Handle file(CreateFileW(path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));ensure(bool(file),"uninstall_shortcut_busy");
    BY_HANDLE_FILE_INFORMATION info{};ensure(GetFileInformationByHandle(file.get(),&info)&&info.nNumberOfLinks==1&&!(attributesOf(file.get())&FILE_ATTRIBUTE_REPARSE_POINT)&&sameText(finalPath(file.get()),expectedFinal(path)),"uninstall_shortcut_unsafe");
    const auto current=readFile(file.get(),1<<20);ensure(current==*bytes&&deleteByHandle(file.get()),"uninstall_shortcut_changed");SHChangeNotify(SHCNE_DELETE,SHCNF_PATHW,path.c_str(),nullptr);
  }catch(const Failure&){throw;}
}
// [graceMs]: during a seamless update the app was already asked to close and is ending its remote
// windows through their normal cleanup (mouse lock, cursor, pending clipboard, Host display);
// wait for that before any WM_CLOSE or termination (a 5 s kill mid-stream leaves the
// relaunched version with a broken cursor and clipboard).
inline void stopClientProcesses(const Roots& roots,DWORD graceMs=0){
  if(graceMs){
    const auto until=GetTickCount64()+graceMs;
    for(;;){
      bool any=false;Handle list(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W e{};e.dwSize=sizeof(e);
      for(BOOL next=list?Process32FirstW(list.get(),&e):FALSE;next&&!any;next=Process32NextW(list.get(),&e)){
        if(!sameText(e.szExeFile,L"Multitap.exe"))continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.th32ProcessID));if(!process)continue;
        std::wstring image(32768,L'\0');DWORD size=DWORD(image.size());if(!QueryFullProcessImageNameW(process.get(),0,image.data(),&size))continue;image.resize(size);
        any=ownedClientTarget(image,clientShortcutRoots(roots));
      }
      if(!any||GetTickCount64()>=until)break;
      Sleep(200);
    }
  }
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));ensure(bool(snapshot),"uninstall_process_list_failed");PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);unsigned count=0;const auto deadline=GetTickCount64()+30000;
  for(BOOL next=Process32FirstW(snapshot.get(),&entry);next;next=Process32NextW(snapshot.get(),&entry)){
    if(!sameText(entry.szExeFile,L"Multitap.exe"))continue;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE|PROCESS_TERMINATE,FALSE,entry.th32ProcessID));if(!process)continue;
    std::wstring image(32768,L'\0');DWORD size=DWORD(image.size());if(!QueryFullProcessImageNameW(process.get(),0,image.data(),&size))continue;image.resize(size);
    if(!ownedClientTarget(image,clientShortcutRoots(roots)))continue;ensure(++count<=128&&GetTickCount64()<deadline,"uninstall_too_many_processes");
    const auto close=[](HWND window,LPARAM raw)->BOOL{DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(pid==DWORD(raw))PostMessageW(window,WM_CLOSE,0,0);return TRUE;};EnumWindows(close,entry.th32ProcessID);
    if(WaitForSingleObject(process.get(),5000)!=WAIT_OBJECT_0){ensure(TerminateProcess(process.get(),0)&&WaitForSingleObject(process.get(),5000)==WAIT_OBJECT_0,"uninstall_client_still_running");}
  }
}
// All admissions complete before removal. A failure retains the ARP entry and
// native uninstaller for a retry; unknown folders and all profile data stay.
inline void removeInstalled(const Roots& roots,const Registration& reg,const Plan& reviewed,const std::function<void()>& beforeUnregister={}){
  const auto fresh=preflight(roots,reg,reviewed.version);ensure(fresh.versions==reviewed.versions&&fresh.uninstaller==reviewed.uninstaller,"uninstall_installation_changed");
  stopClientProcesses(roots);
  // Check every tree again after the processes are gone.
  for(const auto& name:reviewed.versions)ensure(ownedVersion(roots,name),"uninstall_version_changed");
  ensure(fixedClientOwned(roots,reviewed.version),"uninstall_version_changed");verifyTree(rootPath(roots),!roots.qa,0,true);
  bool irreversible=false;
  try{
  removeShortcut(roots.desktop+L"\\Multitap.lnk",[&](const auto& target,const auto&){return ownedClientTarget(target,clientShortcutRoots(roots));});
  for(const auto& name:reviewed.versions){const auto directory=rootPath(roots)+L"\\"+name;irreversible=true;ensure(removeOwnedTree(expectedFinal(directory)),"uninstall_files_in_use");}
  {
    // The fixed folder's own files and folders; Uninstall.exe goes last, below.
    const auto root=rootPath(roots);auto folder=openDirectory(root,READ_CONTROL|FILE_LIST_DIRECTORY);
    for(const auto& entry:listByHandle(folder.get())){if(sameText(entry.name,L"Uninstall.exe"))continue;irreversible=true;ensure(removeOwnedTree(expectedFinal(root+L"\\"+entry.name)),"uninstall_files_in_use");}
  }
  checkRegistration(reg,roots,true);
  // The running removal worker is a verified copy in a protected private
  // staging directory, so the installed maintenance executable is not mapped.
  const auto maintenance=rootPath(roots)+L"\\Uninstall.exe";
  std::vector<char> maintenanceBytes;
  {auto executable=verifiedFile(maintenance,!roots.qa,GENERIC_READ|DELETE);maintenanceBytes=readFile(executable.get());ensure(deleteByHandle(executable.get()),"uninstall_maintenance_busy");}
  try{if(beforeUnregister)beforeUnregister();ensure(RegDeleteKeyExW(reg.hive,reg.key.c_str(),KEY_WOW64_64KEY,0)==ERROR_SUCCESS,"uninstall_registry_remove_failed");}
  catch(...){replaceFile(maintenance,maintenanceBytes);ensure(fileDigest(maintenance,!roots.qa)==reviewed.uninstaller,"uninstall_remover_restore_failed");throw;}
  }catch(...){if(irreversible)throw Failure{"uninstall_partial_removal"};throw;}
}
inline std::wstring knownFolder(REFKNOWNFOLDERID id){PWSTR value=nullptr;ensure(SUCCEEDED(SHGetKnownFolderPath(id,0,nullptr,&value)),"uninstall_known_folder_unavailable");std::wstring out(value);CoTaskMemFree(value);return out;}
inline void privateDirectory(const std::wstring& path){
  PSECURITY_DESCRIPTOR security=nullptr;ensure(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&security,nullptr),"uninstall_stage_security_failed");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};const BOOL created=CreateDirectoryW(path.c_str(),&attributes);const DWORD error=GetLastError();LocalFree(security);ensure(created||error==ERROR_ALREADY_EXISTS,"uninstall_stage_create_failed");plainChain(path,{});auto folder=openDirectory(path,READ_CONTROL);protectedDirectory(folder.get());
}
// The parent is the protected installed uninstaller. Only this verified copy
// may remove it: its two inherited handles are restricted to parent identity/
// exit and readiness. No shell command or delayed reboot deletion is used.
inline void launchRemovalWorker(const Roots& roots){
  const auto installed=rootPath(roots)+L"\\Uninstall.exe";ensure(sameText(modulePath(),installed),"uninstall_installed_entry_required");
  const auto own=fileDigest(installed,true);const auto stageRoot=knownFolder(FOLDERID_ProgramData)+L"\\Multitap-Uninstall";
  privateDirectory(stageRoot);const auto directory=stageRoot+L"\\"+ProductId+L"-"+randomTag();ensure(GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES,"uninstall_stage_exists");privateDirectory(directory);
  const auto worker=directory+L"\\Remove.exe";auto source=verifiedFile(installed,true);const auto bytes=readFile(source.get());replaceFile(worker,bytes);ensure(fileDigest(worker,true)==own,"uninstall_stage_verify_failed");
  Handle parent;HANDLE duplicate=nullptr;ensure(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&duplicate,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0),"uninstall_parent_handle_failed");parent=Handle(duplicate);
  SECURITY_ATTRIBUTES inherited{sizeof(inherited),nullptr,TRUE};Handle ready(CreateEventW(&inherited,TRUE,FALSE,nullptr));ensure(bool(ready),"uninstall_ready_event_failed");
  SIZE_T attributeBytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&attributeBytes);std::vector<BYTE> attributes(attributeBytes);auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());ensure(InitializeProcThreadAttributeList(list,1,0,&attributeBytes),"uninstall_launch_attributes_failed");
  struct Attributes{LPPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}}release{list};HANDLE allowed[]{parent.get(),ready.get()};ensure(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed,sizeof(allowed),nullptr,nullptr),"uninstall_launch_attributes_failed");
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;PROCESS_INFORMATION process{};
  std::wstring command=L"\""+worker+L"\" --uninstall-worker "+std::to_wstring(reinterpret_cast<uintptr_t>(parent.get()))+L" "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.get()));
  ensure(CreateProcessW(worker.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT,nullptr,directory.c_str(),&startup.StartupInfo,&process),"uninstall_worker_launch_failed");Handle child(process.hProcess),thread(process.hThread);HANDLE waits[]{ready.get(),child.get()};
  if(WaitForMultipleObjects(2,waits,FALSE,20000)!=WAIT_OBJECT_0){TerminateProcess(child.get(),ERROR_CANCELLED);throw Failure{"uninstall_worker_not_ready"};}
}
inline std::wstring verifyRemovalWorker(const Roots& roots,HANDLE parent,HANDLE ready){
  const auto module=modulePath(),stageRoot=knownFolder(FOLDERID_ProgramData)+L"\\Multitap-Uninstall";const auto directory=std::filesystem::path(module).parent_path().wstring();const auto leaf=std::filesystem::path(directory).filename().wstring();const auto prefix=std::wstring(ProductId)+L"-";
  ensure(sameText(std::filesystem::path(directory).parent_path().wstring(),stageRoot)&&std::filesystem::path(module).filename()==L"Remove.exe"&&leaf.starts_with(prefix)&&leaf.size()==prefix.size()+12&&leaf.substr(prefix.size()).find_first_not_of(L"0123456789abcdef")==std::wstring::npos,"uninstall_worker_path_invalid");
  plainChain(directory,{});for(const auto& path:{stageRoot,directory}){auto folder=openDirectory(path,READ_CONTROL);protectedDirectory(folder.get());}
  ensure(parent&&parent!=INVALID_HANDLE_VALUE&&ready&&ready!=INVALID_HANDLE_VALUE&&GetProcessId(parent)!=0&&GetProcessId(parent)!=GetCurrentProcessId()&&WaitForSingleObject(parent,0)==WAIT_TIMEOUT,"uninstall_parent_identity_invalid");
  std::wstring image(32768,L'\0');DWORD size=DWORD(image.size());ensure(QueryFullProcessImageNameW(parent,0,image.data(),&size),"uninstall_parent_identity_invalid");image.resize(size);const auto installed=rootPath(roots)+L"\\Uninstall.exe";
  ensure(sameText(image,installed)&&fileDigest(module,true)==fileDigest(installed,true),"uninstall_worker_identity_invalid");
  ensure(SetEvent(ready)&&WaitForSingleObject(parent,15000)==WAIT_OBJECT_0,"uninstall_parent_did_not_exit");return directory;
}
inline void cleanupRemovalWorker(const std::wstring& directory){
  try{auto own=verifiedFile(modulePath(),true,DELETE);if(deleteByHandle(own.get())){own.reset();RemoveDirectoryW(directory.c_str());}}catch(...){ }
}
} // namespace multitap::installation
