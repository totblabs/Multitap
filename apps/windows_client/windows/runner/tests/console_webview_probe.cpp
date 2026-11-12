#include "console_webview_bootstrap.h"
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

// Explicit private probe only. No ShowWindow/SetForegroundWindow/MoveFocus,
// no input, no JavaScript and no console page/guest WebSocket can be loaded.
namespace {
using Microsoft::WRL::ComPtr;using Microsoft::WRL::Callback;
constexpr UINT sessionCheck=WM_APP+7;
std::string ascii(PCWSTR value){std::string result;if(value)for(;*value;++value){if(*value<0x20||*value>0x7e)return {};result+=char(*value);}return result;}
std::wstring wide(const std::string& value){return {value.begin(),value.end()};}
std::string safeErrorCode(std::string_view body){
  if(body.size()>4096)return "unknown";
  const auto skip=[&]{while(!body.empty()&&(body.front()==' '||body.front()=='\r'||body.front()=='\n'||body.front()=='\t'))body.remove_prefix(1);};
  const auto take=[&](std::string_view value){skip();if(body.substr(0,value.size())!=value)return false;body.remove_prefix(value.size());return true;};
  // Only the broker's fixed error.code prefix is decoded; arbitrary response
  // content is never copied to the diagnostic output.
  if(!take("{")||!take("\"error\"")||!take(":")||!take("{")||!take("\"code\"")||!take(":")||!take("\""))return "unknown";
  const auto end=body.find('"');if(end==std::string_view::npos||end>64)return "unknown";
  const auto value=body.substr(0,end);body.remove_prefix(end+1);skip();if(body.empty()||(body.front()!=','&&body.front()!='}'))return "unknown";
  for(const auto* allowed:{"origin_required","origin_not_allowed","console_webview_invalid","console_webview_expired","console_https_required","console_webview_scoped","authentication_required","credentials_change_required","desktop_not_found","console_webview_form_required","body_too_large","request_aborted","invalid_console_path","query_not_supported","console_unavailable","internal_error"})if(value==allowed)return allowed;
  return "unknown";
}
std::string classifyOrigin(bool present,const std::wstring& value,const std::wstring& configured){
  if(!present)return "missing";if(value==configured)return "configured";return value==L"null"?"null":"other";
}
bool samePath(const std::wstring& a,const std::wstring& b){return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;}
struct DirectoryHandle {
  HANDLE value=INVALID_HANDLE_VALUE;
  explicit DirectoryHandle(const std::filesystem::path& path):value(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)){}
  ~DirectoryHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
std::optional<std::wstring> directoryName(HANDLE handle){
  BY_HANDLE_FILE_INFORMATION info{};
  if(handle==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return {};
  const auto count=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!count||count>32768)return {};
  std::wstring value(count,L'\0');const auto written=GetFinalPathNameByHandleW(handle,value.data(),count,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  if(!written||written>=count)return {};value.resize(written);return value;
}
bool removeProfileTree(const std::filesystem::path& path,const std::wstring& resolved,uint64_t deadline,unsigned depth=0){
  if(depth>32||GetTickCount64()>=deadline)return false;
  {
    // Hold this exact directory against rename while visiting its children.
    DirectoryHandle held(path);const auto actual=directoryName(held.value);if(!actual||!samePath(*actual,resolved))return false;
    WIN32_FIND_DATAW entry{};const auto search=FindFirstFileW((path/L"*").c_str(),&entry);if(search==INVALID_HANDLE_VALUE)return false;
    bool ok=true;
    do{
      if(wcscmp(entry.cFileName,L".")==0||wcscmp(entry.cFileName,L"..")==0)continue;
      if(GetTickCount64()>=deadline||(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){ok=false;break;}
      const auto child=path/entry.cFileName;
      if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ok=removeProfileTree(child,resolved+L"\\"+entry.cFileName,deadline,depth+1);
      else ok=DeleteFileW(child.c_str())!=FALSE;
      if(!ok)break;
    }while(FindNextFileW(search,&entry));
    const auto end=GetLastError();FindClose(search);if(!ok||end!=ERROR_NO_MORE_FILES)return false;
  }
  // RemoveDirectory never recursively follows a replacement after the hold.
  return RemoveDirectoryW(path.c_str())!=FALSE;
}
bool removeOwnedProfile(const std::filesystem::path& root,const std::filesystem::path& leaf){
  if(!root.is_absolute()||root.filename()!=L"MultitapConsoleProbe"||leaf.parent_path()!=root)return false;
  GUID id{};const auto name=leaf.filename().wstring();wchar_t canonical[40]{};
  if(name.size()!=38||FAILED(CLSIDFromString(name.c_str(),&id))||!StringFromGUID2(id,canonical,40)||!samePath(name,canonical))return false;
  DirectoryHandle heldRoot(root);const auto resolvedRoot=directoryName(heldRoot.value);if(!resolvedRoot)return false;
  // Reject redirected roots as well as a reparse point in the GUID leaf/tree.
  const auto expectedRoot=L"\\\\?\\"+root.lexically_normal().wstring();if(!samePath(*resolvedRoot,expectedRoot))return false;
  const auto deadline=GetTickCount64()+5000;
  do{
    if(removeProfileTree(leaf,*resolvedRoot+L"\\"+name,deadline)){
      const auto attributes=GetFileAttributesW(leaf.c_str()),error=GetLastError();
      return attributes==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND);
    }
    if(GetTickCount64()>=deadline)break;Sleep(25);
  }while(true);
  return false;
}
struct Probe {
  multitap::ConsoleWebViewTarget target;HWND hwnd=nullptr;bool done=false,failed=false,closing=false,inPrivate=false,groupExited=false,attachOriginHandled=false;int attachStatus=0,sessionStatus=0;UINT32 exitedGroupPid=0;
  std::string sentOrigin="missing",attachErrorCode="none";
  EventRegistrationToken exitToken{};
  ComPtr<ICoreWebView2Environment> environment;ComPtr<ICoreWebView2Controller> controller;ComPtr<ICoreWebView2> view;
  ComPtr<ICoreWebView2Environment5> exitEnvironment;
  LRESULT message(UINT message){
    if(message==sessionCheck&&view&&!closing){view->Navigate(wide(target.origin+"/console/api/session").c_str());return 0;}return 0;
  }
  void start(const std::wstring& directory){
    auto options=Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();ComPtr<ICoreWebView2EnvironmentOptions7> channels;options.As(&channels);channels->put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS_STABLE);
    const auto result=CreateCoreWebView2EnvironmentWithOptions(nullptr,directory.c_str(),options.Get(),Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT hr,ICoreWebView2Environment* env)->HRESULT{
      if(closing)return S_OK;
      if(FAILED(hr)||!env){failed=done=true;return S_OK;}environment=env;
      if(FAILED(environment.As(&exitEnvironment))||FAILED(exitEnvironment->add_BrowserProcessExited(Callback<ICoreWebView2BrowserProcessExitedEventHandler>([this](ICoreWebView2Environment*,ICoreWebView2BrowserProcessExitedEventArgs* args)->HRESULT{
        UINT32 pid=0;if(SUCCEEDED(args->get_BrowserProcessId(&pid))&&pid){exitedGroupPid=pid;groupExited=true;}return S_OK;
      }).Get(),&exitToken))){failed=done=true;return S_OK;}
      ComPtr<ICoreWebView2Environment10> privateEnv;ComPtr<ICoreWebView2ControllerOptions> control;
      if(FAILED(env->QueryInterface(IID_PPV_ARGS(&privateEnv)))||FAILED(privateEnv->CreateCoreWebView2ControllerOptions(&control))||FAILED(control->put_IsInPrivateModeEnabled(TRUE))){failed=done=true;return S_OK;}
      control->put_ProfileName(L"PrivateBootstrapProbe");
      const auto created=privateEnv->CreateCoreWebView2ControllerWithOptions(hwnd,control.Get(),Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this](HRESULT status,ICoreWebView2Controller* value)->HRESULT{
        if(closing){if(value)value->Close();return S_OK;}
        if(FAILED(status)||!value){failed=done=true;return S_OK;}controller=value;controller->put_IsVisible(FALSE);RECT bounds{0,0,640,480};controller->put_Bounds(bounds);
        if(FAILED(value->get_CoreWebView2(&view))){failed=done=true;return S_OK;}
        ComPtr<ICoreWebView2_13> profiles;ComPtr<ICoreWebView2Profile> profile;BOOL privateMode=FALSE;
        if(FAILED(view.As(&profiles))||FAILED(profiles->get_Profile(&profile))||FAILED(profile->get_IsInPrivateModeEnabled(&privateMode))||!privateMode){failed=done=true;return S_OK;}
        inPrivate=true;
        ComPtr<ICoreWebView2Settings> settings;if(FAILED(view->get_Settings(&settings))||FAILED(settings->put_IsScriptEnabled(FALSE))||FAILED(settings->put_AreHostObjectsAllowed(FALSE))||FAILED(settings->put_IsWebMessageEnabled(FALSE))||FAILED(settings->put_AreDevToolsEnabled(FALSE))){failed=done=true;return S_OK;}
        EventRegistrationToken token{};
        if(FAILED(view->AddWebResourceRequestedFilter(wide(target.attach).c_str(),COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT))||
           FAILED(view->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([this](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args)->HRESULT{
             ComPtr<ICoreWebView2WebResourceRequest> request;COREWEBVIEW2_WEB_RESOURCE_CONTEXT context{};
             const auto restored=SUCCEEDED(args->get_Request(&request))&&SUCCEEDED(args->get_ResourceContext(&context))?
               multitap::RestoreConsoleBootstrapOrigin(request.Get(),target,context,attachOriginHandled):E_FAIL;
             if(FAILED(restored)){
               ComPtr<ICoreWebView2WebResourceResponse> denied;
               if(SUCCEEDED(environment->CreateWebResourceResponse(nullptr,403,L"Forbidden",L"Cache-Control: no-store",&denied)))args->put_Response(denied.Get());
               failed=done=true;
             }
             return S_OK;
           }).Get(),&token))){failed=done=true;return S_OK;}
        view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([this](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT{
          LPWSTR raw=nullptr;args->get_Uri(&raw);const auto uri=ascii(raw);CoTaskMemFree(raw);
          if(uri==target.page){args->put_Cancel(TRUE);PostMessageW(hwnd,sessionCheck,0,0);}
          else if(uri!=target.attach&&uri!=target.origin+"/console/api/session"){args->put_Cancel(TRUE);failed=done=true;}
          return S_OK;
        }).Get(),&token);
        ComPtr<ICoreWebView2_2> responses;if(FAILED(view.As(&responses))){failed=done=true;return S_OK;}
        responses->add_WebResourceResponseReceived(Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>([this](ICoreWebView2*,ICoreWebView2WebResourceResponseReceivedEventArgs* args)->HRESULT{
          ComPtr<ICoreWebView2WebResourceRequest> request;ComPtr<ICoreWebView2WebResourceResponseView> response;LPWSTR raw=nullptr;
          if(FAILED(args->get_Request(&request))||FAILED(request->get_Uri(&raw))||FAILED(args->get_Response(&response))){CoTaskMemFree(raw);failed=done=true;return S_OK;}
          const auto uri=ascii(raw);CoTaskMemFree(raw);int code=0;response->get_StatusCode(&code);
          if(uri==target.attach){
            attachStatus=code;ComPtr<ICoreWebView2HttpRequestHeaders> headers;BOOL contains=FALSE;LPWSTR origin=nullptr;
            if(SUCCEEDED(request->get_Headers(&headers))&&SUCCEEDED(headers->Contains(L"Origin",&contains))){
              if(!contains)sentOrigin="missing";
              else if(SUCCEEDED(headers->GetHeader(L"Origin",&origin))&&origin)sentOrigin=classifyOrigin(true,origin,wide(target.origin));
              else sentOrigin="other";
            }else sentOrigin="other";
            CoTaskMemFree(origin);
            if(code==403){
              attachErrorCode="unknown";
              const auto pending=response->GetContent(Callback<ICoreWebView2WebResourceResponseViewGetContentCompletedHandler>([this](HRESULT read,IStream* content)->HRESULT{
                if(closing)return S_OK;
                if(SUCCEEDED(read)&&content){char buffer[4097]{};ULONG bytes=0;
                  if(SUCCEEDED(content->Read(buffer,sizeof(buffer),&bytes))&&bytes<=4096)attachErrorCode=safeErrorCode(std::string_view(buffer,bytes));
                  SecureZeroMemory(buffer,sizeof(buffer));
                }
                failed=done=true;return S_OK;
              }).Get());
              if(FAILED(pending))failed=done=true;
            }else if(code!=303)failed=done=true;
          }
          if(uri==target.origin+"/console/api/session"){sessionStatus=code;failed=code!=200;done=true;}
          return S_OK;
        }).Get(),&token);
        if(FAILED(multitap::NavigateConsoleBootstrap(environment.Get(),view.Get(),target)))failed=done=true;
        return S_OK;
      }).Get());
      if(FAILED(created))failed=done=true;return S_OK;
    }).Get());
    if(FAILED(result))failed=done=true;
  }
};
LRESULT CALLBACK message(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
  auto* probe=reinterpret_cast<Probe*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if(msg==WM_NCCREATE){probe=static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(probe));}
  if(msg==sessionCheck&&probe)return probe->message(msg);
  return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int main(int argc,char** argv){
  if(argc==2&&std::string(argv[1])=="--diagnostic-self-test"){
    int checks=0,failures=0;const auto check=[&](bool value){++checks;if(!value)++failures;};
    check(safeErrorCode("{\"error\":{\"code\":\"origin_required\",\"message\":\"ignored\"}}") == "origin_required");
    check(safeErrorCode(" { \"error\" : { \"code\" : \"console_webview_expired\" } }") == "console_webview_expired");
    for(const auto* invalid:{"{\"code\":\"origin_required\"}","{\"message\":{\"code\":\"origin_required\"}}","{\"error\":{\"code\":\"untrusted_secret\"}}","{\"error\":{\"code\":\"origin_\\u0072equired\"}}","<html>ignored</html>"})check(safeErrorCode(invalid)=="unknown");
    check(safeErrorCode(std::string(4097,'x'))=="unknown");
    check(classifyOrigin(false,L"",L"https://example.test")=="missing");check(classifyOrigin(true,L"null",L"https://example.test")=="null");
    check(classifyOrigin(true,L"https://example.test",L"https://example.test")=="configured");check(classifyOrigin(true,L"https://other.test",L"https://example.test")=="other");
    std::cout<<"{\"diagnosticChecks\":"<<checks<<",\"passed\":"<<(failures?"false":"true")<<",\"browserCreated\":false}\n";return failures?1:0;
  }
  if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
  LPWSTR version=nullptr;const auto available=GetAvailableCoreWebView2BrowserVersionString(nullptr,&version);const bool runtime=SUCCEEDED(available)&&version;
  if(argc==2&&std::string(argv[1])=="--runtime"){std::cout<<"{\"runtimeAvailable\":"<<(runtime?"true":"false")<<",\"version\":\""<<ascii(version)<<"\",\"browserCreated\":false}\n";CoTaskMemFree(version);CoUninitialize();return runtime?0:1;}
  CoTaskMemFree(version);
  if(argc!=2||std::string(argv[1])!="--bootstrap"||!runtime){CoUninitialize();return 2;}
  std::string origin,id,path,code;std::getline(std::cin,origin);std::getline(std::cin,id);std::getline(std::cin,path);std::getline(std::cin,code);
  auto target=multitap::ConsoleWebViewDestination(origin,id,path,code);if(!code.empty())SecureZeroMemory(code.data(),code.size());code.clear();
  if(!target){CoUninitialize();return 2;}
  Probe probe;probe.target=std::move(*target);
  WNDCLASSW type{};type.lpfnWndProc=message;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"Multitap.PrivateConsoleBootstrapProbe";RegisterClassW(&type);
  probe.hwnd=CreateWindowExW(0,type.lpszClassName,L"",WS_OVERLAPPED,0,0,640,480,nullptr,nullptr,type.hInstance,&probe);
  GUID guid{};CoCreateGuid(&guid);wchar_t unique[40]{};StringFromGUID2(guid,unique,40);
  const auto profileRoot=std::filesystem::temp_directory_path().lexically_normal()/L"MultitapConsoleProbe";
  const auto directory=profileRoot/unique;
  std::error_code error;std::filesystem::create_directories(directory,error);if(error||!probe.hwnd){CoUninitialize();return 2;}
  probe.start(directory.wstring());const auto deadline=GetTickCount64()+25000;
  while(!probe.done&&GetTickCount64()<deadline){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}MsgWaitForMultipleObjectsEx(0,nullptr,25,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
  const bool visible=IsWindowVisible(probe.hwnd)!=FALSE;const bool ok=probe.done&&!probe.failed&&probe.inPrivate&&probe.sessionStatus==200&&!visible;
  UINT32 browserPid=0;HANDLE browser=nullptr;
  if(probe.view&&SUCCEEDED(probe.view->get_BrowserProcessId(&browserPid))&&browserPid)browser=OpenProcess(SYNCHRONIZE,FALSE,browserPid);
  probe.closing=true;if(probe.controller)probe.controller->Close();probe.view.Reset();probe.controller.Reset();DestroyWindow(probe.hwnd);
  bool browserExited=false;const auto cleanupDeadline=GetTickCount64()+15000;
  while(browser&&GetTickCount64()<cleanupDeadline){
    if(WaitForSingleObject(browser,0)==WAIT_OBJECT_0)browserExited=true;
    if(browserExited&&probe.groupExited&&probe.exitedGroupPid==browserPid)break;
    MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    MsgWaitForMultipleObjectsEx(browserExited?0:1,browserExited?nullptr:&browser,25,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  }
  if(browser)CloseHandle(browser);
  const bool groupExited=probe.groupExited&&probe.exitedGroupPid==browserPid&&browserPid!=0;
  if(probe.exitEnvironment)probe.exitEnvironment->remove_BrowserProcessExited(probe.exitToken);
  probe.exitEnvironment.Reset();probe.environment.Reset();
  CoUninitialize();
  const bool profileRemoved=browserExited&&groupExited&&removeOwnedProfile(profileRoot,directory);
  std::cout<<"{\"attachHttpStatus\":"<<probe.attachStatus<<",\"sessionHttpStatus\":"<<probe.sessionStatus<<",\"timedOut\":"<<(!probe.done?"true":"false")<<",\"windowVisible\":"<<(visible?"true":"false")<<",\"inPrivateMode\":"<<(probe.inPrivate?"true":"false")<<",\"browserProcessExitObserved\":"<<(browserExited?"true":"false")<<",\"browserProcessGroupExitObserved\":"<<(groupExited?"true":"false")<<",\"profileRemoved\":"<<(profileRemoved?"true":"false")<<",\"guestSessionRequested\":false,\"javascriptEnabled\":false,\"sentOrigin\":\""<<probe.sentOrigin<<"\",\"attachErrorCode\":\""<<probe.attachErrorCode<<"\"}\n";
  return ok&&browserExited&&groupExited&&profileRemoved?0:1;
}
