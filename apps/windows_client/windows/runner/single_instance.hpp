#pragma once
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace multitap {
// Stable across versions. Host settings and the desktop client are separate
// products; session windows remain children of their one client process.
class SingleInstance {
 public:
  ~SingleInstance(){if(mutex_)CloseHandle(mutex_);if(namespace_)ClosePrivateNamespace(namespace_,0);}
  SingleInstance(const SingleInstance&)=delete;
  SingleInstance& operator=(const SingleInstance&)=delete;
  explicit SingleInstance(std::wstring scope=L"Multitap.Instance",bool legacy=true):scope_(std::move(scope)),legacy_(legacy){}
  // A predictable Local\ instance name is one that another account in the
  // session could create first (with a DACL that denies us), so the client
  // could not start. The mutex therefore lives
  // in a private namespace whose boundary is this user's SID and this session:
  // only a token holding that SID can create the namespace, and its DACL admits
  // only this user and SYSTEM. The released Local\ name is used only if the
  // private namespace is unavailable. Older and newer clients still find each
  // other through the window property below.
  bool Acquire(bool host) {
    host_=host;property_=scope_+(host?L".HostSettings":L".Client");
    HANDLE token=nullptr;DWORD size=0;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> data(size);
    const bool valid=GetTokenInformation(token,TokenUser,data.data(),size,&size)!=FALSE;CloseHandle(token);
    if(!valid)return false;LPWSTR sid=nullptr;
    const PSID user=reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    if(!ConvertSidToStringSidW(user,&sid))return false;
    const std::wstring userSid(sid);LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;GA;;;SY)(A;;GA;;;"+userSid+L")").c_str(),SDDL_REVISION_1,&descriptor,nullptr))return false;
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const auto name=OpenNamespace(user,security)?std::wstring(kAlias)+L"\\"+property_:std::wstring(L"Local\\")+property_+L"."+userSid;
    mutex_=CreateMutexW(&security,FALSE,name.c_str());const DWORD created=GetLastError();LocalFree(descriptor);
    SetLastError(created);if(!mutex_)return false;
    const bool duplicate=GetLastError()==ERROR_ALREADY_EXISTS;
    if(duplicate){for(int attempt=0;attempt<40;++attempt){if(ActivateExisting(false))break;Sleep(50);}return false;}
    // Older installed releases did not own this mutex. Activate the existing
    // verified installed product instead of starting another version beside it.
    return !ActivateExisting(legacy_);
  }
  void Register(HWND window){SetPropW(window,property_.c_str(),reinterpret_cast<HANDLE>(ULONG_PTR(GetCurrentProcessId())));}
  bool PrivateNamespace()const{return private_;}
 private:
  static constexpr wchar_t kAlias[]=L"Multitap";
  HANDLE mutex_=nullptr,namespace_=nullptr;bool host_=false,private_=false;std::wstring scope_,property_;bool legacy_=true;
  // This user's private namespace for this session (created by the first
  // instance, opened by later processes). A process that already holds it
  // (ERROR_DUP_NAME: e.g. the client and its settings scope in one process)
  // uses the alias it has.
  bool OpenNamespace(PSID user,SECURITY_ATTRIBUTES& security){
    DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session))return false;
    HANDLE boundary=CreateBoundaryDescriptorW((L"Multitap.Instance.Session."+std::to_wstring(session)).c_str(),0);
    if(!boundary)return false;
    if(AddSIDToBoundaryDescriptor(&boundary,user)){
      namespace_=CreatePrivateNamespaceW(&security,boundary,kAlias);
      if(!namespace_&&GetLastError()==ERROR_ALREADY_EXISTS){
        namespace_=OpenPrivateNamespaceW(boundary,kAlias);
        private_=namespace_||GetLastError()==ERROR_DUP_NAME;
      } else private_=namespace_!=nullptr;
    }
    DeleteBoundaryDescriptor(boundary);
    return private_;
  }
  struct Search {SingleInstance* owner;bool legacy;HWND found=nullptr;};
  bool InstalledClient(DWORD pid)const {
    if(host_)return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return false;
    wchar_t image[32768]{};DWORD length=32768;const bool read=QueryFullProcessImageNameW(process,0,image,&length)!=FALSE;CloseHandle(process);
    if(!read)return false;
    PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))return false;
    const auto root=std::filesystem::path(local)/L"Multitap"/L"Client";CoTaskMemFree(local);
    const std::filesystem::path path(image);const auto version=path.parent_path().filename().wstring();
    if(_wcsicmp(path.filename().c_str(),L"Multitap.exe")||_wcsicmp(path.parent_path().parent_path().c_str(),root.c_str())||!std::regex_match(version,std::wregex(L"[0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}")))return false;
    for(auto check=path;check!=root.parent_path();check=check.parent_path()){
      const DWORD flags=GetFileAttributesW(check.c_str());if(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    const auto marker=path.parent_path()/L"multitap-client.json";
    const DWORD flags=GetFileAttributesW(marker.c_str());if(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    std::error_code error;const auto bytes=std::filesystem::file_size(marker,error);if(error||bytes>4096)return false;
    std::ifstream file(marker,std::ios::binary);const std::string text((std::istreambuf_iterator<char>(file)),{});
    return std::regex_search(text,std::regex("\"product\"\\s*:\\s*\"Multitap\""));
  }
  bool ActivateExisting(bool legacy) {
    Search search{this,legacy};
    EnumWindows([](HWND window,LPARAM parameter)->BOOL{
      auto& search=*reinterpret_cast<Search*>(parameter);DWORD pid=0;GetWindowThreadProcessId(window,&pid);
      if(!pid||pid==GetCurrentProcessId()||GetWindow(window,GW_OWNER))return TRUE;
      wchar_t cls[64]{};GetClassNameW(window,cls,64);if(wcscmp(cls,L"MULTITAP_CLIENT_WINDOW"))return TRUE;
      if(!GetPropW(window,search.owner->property_.c_str())&&(!search.legacy||!search.owner->InstalledClient(pid)))return TRUE;
      search.found=window;return FALSE;
    },reinterpret_cast<LPARAM>(&search));
    if(!search.found)return false;
    ShowWindow(search.found,IsIconic(search.found)?SW_RESTORE:SW_SHOW);SetForegroundWindow(search.found);return true;
  }
};
}
