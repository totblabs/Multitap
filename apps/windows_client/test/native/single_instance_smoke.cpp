#include "single_instance.hpp"
#include <functional>
#include <iostream>
namespace {
std::wstring CurrentUserSid(){
  HANDLE token=nullptr;DWORD size=0;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return {};
  GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> data(size);
  const bool valid=GetTokenInformation(token,TokenUser,data.data(),size,&size)!=FALSE;CloseHandle(token);
  LPWSTR sid=nullptr;if(!valid||!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid))return {};
  std::wstring value(sid);LocalFree(sid);return value;
}
// The first instance of a client that rests in the notification area: its main
// window exists but is hidden (close-to-tray). Another launch
// must bring that window back, also when it was minimized before it was hidden.
int TrayOwner(const wchar_t* scope,const wchar_t* first,const wchar_t* second){
  const auto parse=[](const wchar_t* text){return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(std::wcstoull(text,nullptr,16)));};
  HANDLE ready=parse(first),prepared=parse(second);
  WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MULTITAP_CLIENT_WINDOW";
  RegisterClassW(&cls);
  HWND window=CreateWindowExW(0,cls.lpszClassName,L"Multitap",WS_OVERLAPPEDWINDOW,0,0,400,300,nullptr,nullptr,cls.hInstance,nullptr);
  if(!window)return 20;
  multitap::SingleInstance instance(scope,false);
  if(!instance.Acquire(false))return 21;
  instance.Register(window);
  if(IsWindowVisible(window))return 22;                      // hidden, as in the tray
  SetEvent(ready);
  const auto pump=[&](const std::function<bool()>& done){
    const ULONGLONG deadline=GetTickCount64()+15000;MSG msg;
    while(!done()&&GetTickCount64()<deadline){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}
    return done();
  };
  if(!pump([&]{return IsWindowVisible(window)!=FALSE;}))return 23;   // 1: restored from hidden
  // Minimized, then hidden (X on a minimized window): the next launch restores it.
  ShowWindow(window,SW_MINIMIZE);ShowWindow(window,SW_HIDE);
  if(IsWindowVisible(window)||!IsIconic(window))return 24;
  SetEvent(prepared);
  if(!pump([&]{return IsWindowVisible(window)!=FALSE;}))return 25;   // 2: restored from hidden+minimized
  if(IsIconic(window))return 26;
  return 0;
}
}
int wmain(int argc,wchar_t** argv){
  // Child mode: a second process of this user and session.
  if(argc==3&&std::wstring(argv[1])==L"--child"){multitap::SingleInstance child(argv[2],false);return child.Acquire(false)?0:10;}
  if(argc==5&&std::wstring(argv[1])==L"--tray-owner")return TrayOwner(argv[2],argv[3],argv[4]);
  const auto scope=L"Multitap.QA.Instance."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
  {
    multitap::SingleInstance first(scope,false),second(scope,false),host(scope,false);
    if(!first.Acquire(false)||second.Acquire(false)||!host.Acquire(true))return 1;
    if(!first.PrivateNamespace()||!host.PrivateNamespace())return 3;
  }
  multitap::SingleInstance reopened(scope,false);
  if(!reopened.Acquire(false))return 2;
  // Another process opens the same private namespace and sees the instance.
  {
    wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);
    std::wstring command=L"\""+std::wstring(self)+L"\" --child "+scope;STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
    if(!CreateProcessW(self,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return 8;
    WaitForSingleObject(child.hProcess,15000);DWORD code=0;GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hThread);CloseHandle(child.hProcess);
    if(code!=10)return 9;
  }
  // The released name, squatted with a DACL that denies everyone (as another
  // account in the session could): the instance still starts.
  const auto squatScope=scope+L".Squat";
  PSECURITY_DESCRIPTOR deny=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P",SDDL_REVISION_1,&deny,nullptr))return 4;
  SECURITY_ATTRIBUTES denied{sizeof(denied),deny,FALSE};
  const auto legacyName=L"Local\\"+squatScope+L".Client."+CurrentUserSid();
  HANDLE squat=CreateMutexW(&denied,FALSE,legacyName.c_str());LocalFree(deny);
  if(!squat)return 5;
  HANDLE blocked=CreateMutexW(nullptr,FALSE,legacyName.c_str());
  const bool legacyBlocked=!blocked&&GetLastError()==ERROR_ACCESS_DENIED;if(blocked)CloseHandle(blocked);
  multitap::SingleInstance squatted(squatScope,false);
  const bool started=squatted.Acquire(false);CloseHandle(squat);
  if(!legacyBlocked||!started)return 6;
  // A boundary naming an account this token does not hold (SYSTEM) cannot be
  // created: another account cannot pre-create this user's namespace.
  bool foreignRefused=false;
  {
    BYTE system[SECURITY_MAX_SID_SIZE]{};DWORD length=sizeof(system);
    HANDLE boundary=CreateBoundaryDescriptorW(L"Multitap.QA.ForeignBoundary",0);
    if(boundary&&CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&length)&&AddSIDToBoundaryDescriptor(&boundary,system)){
      HANDLE foreign=CreatePrivateNamespaceW(nullptr,boundary,L"Multitap.QA.Foreign");
      foreignRefused=foreign==nullptr;if(foreign)ClosePrivateNamespace(foreign,PRIVATE_NAMESPACE_FLAG_DESTROY);
    }
    if(boundary)DeleteBoundaryDescriptor(boundary);
  }
  if(!foreignRefused)return 7;
  // A client resting in the tray (hidden main window) is brought back by a second launch.
  // The windows live on a private desktop, so nothing flashes on the user's screen.
  {
    const auto name=L"Multitap.InstanceQa."+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop)return 10;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE ready=CreateEventW(&inherit,TRUE,FALSE,nullptr),prepared=CreateEventW(&inherit,TRUE,FALSE,nullptr);
    wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);
    const auto hiddenScope=scope+L".Tray";
    const auto hex=[](HANDLE h){wchar_t text[32];swprintf_s(text,L"%llx",static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(h)));return std::wstring(text);};
    std::wstring command=L"\""+std::wstring(self)+L"\" --tray-owner "+hiddenScope+L" "+hex(ready)+L" "+hex(prepared);
    std::wstring station=L"WinSta0\\"+name;STARTUPINFOW startup{sizeof(startup)};startup.lpDesktop=station.data();PROCESS_INFORMATION child{};
    if(!CreateProcessW(self,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return 11;
    if(WaitForSingleObject(ready,15000)!=WAIT_OBJECT_0)return 12;
    if(!SetThreadDesktop(desktop))return 13;
    multitap::SingleInstance later(hiddenScope,false);
    if(later.Acquire(false))return 14;                      // the running one is found, even hidden
    if(WaitForSingleObject(prepared,15000)!=WAIT_OBJECT_0)return 15;
    multitap::SingleInstance again(hiddenScope,false);
    if(again.Acquire(false))return 16;                      // and when it was minimized before it was hidden
    WaitForSingleObject(child.hProcess,15000);DWORD code=99;GetExitCodeProcess(child.hProcess,&code);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);CloseHandle(ready);CloseHandle(prepared);
    if(code!=0)return 100+static_cast<int>(code);
  }
  std::cout<<"{\"hiddenWindowRestored\":true,\"stableMutex\":true,\"duplicateRejected\":true,\"hostSettingsIndependent\":true,\"releasedOnExit\":true,\"privateNamespace\":true,\"secondProcessRejected\":true,\"squattedLegacyNameStillStarts\":true,\"foreignBoundaryRefused\":true,\"productionMutexUsed\":false,\"productProcessesClosed\":0}\n";
}
