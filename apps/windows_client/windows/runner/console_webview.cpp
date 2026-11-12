#include "console_webview.h"
#include "caption_theme.h"
#include "console_webview_bootstrap.h"
#include "console_webview_close.h"
#include "resource.h"
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <algorithm>
#include <map>
#include <thread>
#include <vector>
#include "ui_language.h"

namespace multitap {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t windowClass[]=L"Multitap.Console.WebView";
constexpr UINT_PTR closeAnswerTimer=1;
constexpr UINT closeConfirmed=WM_APP+41;
// The console is a top-level window of its own, like the stream window: the
// launcher may sit above it. Closing a top-level window passes activation to
// whatever is next in z-order, often another program while the launcher sits
// further back. So the console gives it to the launcher first, while it still
// holds the foreground, and only then is destroyed (destroying an inactive
// window moves activation nowhere).
void returnToOwner(HWND window,HWND app){
  if(!app||!IsWindowVisible(app)||GetForegroundWindow()!=window)return;
  if(IsIconic(app))ShowWindow(app,SW_RESTORE);
  SetForegroundWindow(app);
}
std::wstring wide(const std::string& value){return {value.begin(),value.end()};}
std::string ascii(PCWSTR value){
  std::string out;if(!value)return out;
  for(;*value;++value){if(*value<0x20||*value>0x7e)return {};out.push_back(char(*value));}return out;
}
std::string takeUri(LPWSTR value){const auto text=ascii(value);CoTaskMemFree(value);return text;}
void clearCode(std::string& code){if(!code.empty())SecureZeroMemory(code.data(),code.size());code.clear();}
std::filesystem::path profileFolder(){
  PWSTR raw=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&raw)))return {};
  std::filesystem::path base(raw);CoTaskMemFree(raw);GUID id{};if(FAILED(CoCreateGuid(&id)))return {};
  wchar_t name[40]{};if(!StringFromGUID2(id,name,40))return {};
  auto root=base/L"Multitap"/L"console-webview";
  std::error_code error;std::filesystem::create_directories(root,error);if(error)return {};
  auto folder=root/name;
  if(!std::filesystem::create_directory(folder,error)||error)return {};
  return folder;
}
void removeProfile(const std::filesystem::path& folder){
  // Only our unique profile leaf may be removed; never follow a replaced root.
  if(folder.empty()||folder.parent_path().filename()!=L"console-webview"||folder.filename().native().size()!=38)return;
  for(auto path=folder;!path.empty();path=path.parent_path()){
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
    if(path==path.root_path())break;
  }
  std::error_code error;std::filesystem::remove_all(folder,error);
}
}

std::string ConsoleWebViewOwnerId(const std::string& code){
  UCHAR digest[32]{};
  if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(code.data())),ULONG(code.size()),digest,sizeof(digest))!=0)return {};
  constexpr char hex[]="0123456789abcdef";std::string value;value.reserve(64);
  for(const auto byte:digest){value+=hex[byte>>4];value+=hex[byte&15];}return value;
}

HRESULT NavigateConsoleBootstrap(ICoreWebView2Environment* environment,ICoreWebView2* view,ConsoleWebViewTarget& target){
  ComPtr<ICoreWebView2Environment2> requests;ComPtr<ICoreWebView2_2> navigation;
  HRESULT result=environment->QueryInterface(IID_PPV_ARGS(&requests));
  if(SUCCEEDED(result))result=view->QueryInterface(IID_PPV_ARGS(&navigation));
  std::string body="code="+target.code;ComPtr<IStream> stream;
  if(SUCCEEDED(result)){stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()),UINT(body.size())));if(!stream)result=E_OUTOFMEMORY;}
  const auto headers=L"Content-Type: application/x-www-form-urlencoded\r\nOrigin: "+wide(target.origin)+L"\r\nCache-Control: no-store";
  ComPtr<ICoreWebView2WebResourceRequest> request;
  if(SUCCEEDED(result))result=requests->CreateWebResourceRequest(wide(target.attach).c_str(),L"POST",stream.Get(),headers.c_str(),&request);
  clearCode(body);clearCode(target.code);
  if(SUCCEEDED(result))result=navigation->NavigateWithWebResourceRequest(request.Get());
  return result;
}

struct ConsoleWebViews::Impl:std::enable_shared_from_this<Impl> {
  struct Window:std::enable_shared_from_this<Window> {
    std::weak_ptr<Impl> owner;
    ConsoleWebViewTarget target;
    std::string owner_id;
    HWND hwnd=nullptr;
    std::filesystem::path folder;
    std::wstring status=multitap::Tr(L"Console을 열고 있어요.", L"Opening Console.");
    bool bootstrap=true,closing=false,attachOriginHandled=false,dark=true,fullscreen=false;
    ConsoleCloseFlow closeFlow;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};LONG_PTR framedStyle=0;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    ComPtr<ICoreWebView2Profile> profile;
    // An open close request still falls back to the native question on its timer.
    void fail(const wchar_t* text){status=text;clearCode(target.code);closeFlow.pageReady=false;setFullscreen(false);if(controller)controller->Close();view.Reset();controller.Reset();profile.Reset();if(hwnd)InvalidateRect(hwnd,nullptr,TRUE);}
    // The page follows the app theme through its colour scheme; the caption
    // and the area behind the page match the launcher's.
    void theme(){
      if(!hwnd)return;ApplyCaptionTheme(hwnd,dark);
      if(profile)profile->put_PreferredColorScheme(dark?COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK:COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
      ComPtr<ICoreWebView2Controller2> background;
      if(controller&&SUCCEEDED(controller.As(&background)))background->put_DefaultBackgroundColor(dark?COREWEBVIEW2_COLOR{255,14,18,22}:COREWEBVIEW2_COLOR{255,238,241,244});
      InvalidateRect(hwnd,nullptr,TRUE);
    }
    void closeNow(){if(!hwnd)return;KillTimer(hwnd,closeAnswerTimer);if(const auto app=owner.lock())returnToOwner(hwnd,app->owner);DestroyWindow(hwnd);}
    void step(ConsoleCloseFlow::Step next){
      switch(next){
        case ConsoleCloseFlow::Step::AskPage:
          if(view&&SUCCEEDED(view->PostWebMessageAsString(kConsoleCloseRequest))){SetTimer(hwnd,closeAnswerTimer,1500,nullptr);return;}
          step(closeFlow.Unanswered());return;
        case ConsoleCloseFlow::Step::AskNative:
          KillTimer(hwnd,closeAnswerTimer);
          if(MessageBoxW(hwnd,multitap::Tr(L"Console을 닫을까요?\n데스크톱과 실행 중인 작업은 그대로 유지돼요.", L"Close Console?\nThe desktop and running work stay as they are."),L"Multitap",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES)closeNow();
          return;
        // Out of the WebView's own event first.
        case ConsoleCloseFlow::Step::Close:PostMessageW(hwnd,closeConfirmed,0,0);return;
        case ConsoleCloseFlow::Step::Answered:KillTimer(hwnd,closeAnswerTimer);return;
        default:return;
      }
    }
    // The page's 전체 화면 fills this window's monitor, as a browser would.
    void setFullscreen(bool on){
      if(!hwnd||on==fullscreen)return;
      if(on){
        MONITORINFO monitor{sizeof(monitor)};placement.length=sizeof(placement);
        if(!GetWindowPlacement(hwnd,&placement)||!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))return;
        framedStyle=GetWindowLongPtrW(hwnd,GWL_STYLE);fullscreen=true;
        SetWindowLongPtrW(hwnd,GWL_STYLE,framedStyle&~WS_OVERLAPPEDWINDOW);
        const auto& area=monitor.rcMonitor;SetWindowPos(hwnd,HWND_TOP,area.left,area.top,area.right-area.left,area.bottom-area.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
      }else{
        fullscreen=false;SetWindowLongPtrW(hwnd,GWL_STYLE,framedStyle);SetWindowPlacement(hwnd,&placement);
        SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
      }
    }
    void size(){if(controller&&hwnd){RECT bounds{};GetClientRect(hwnd,&bounds);controller->put_Bounds(bounds);}}
    void initialize(){
      folder=profileFolder();if(folder.empty()){fail(multitap::Tr(L"Console을 시작하지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't start Console.\nClose the window and connect again."));return;}
      auto options=Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
      ComPtr<ICoreWebView2EnvironmentOptions7> channels;
      if(SUCCEEDED(options.As(&channels)))channels->put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS_STABLE);
      const std::weak_ptr<Window> weak=shared_from_this();
      const auto result=CreateCoreWebView2EnvironmentWithOptions(nullptr,folder.c_str(),options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([weak](HRESULT error,ICoreWebView2Environment* env)->HRESULT{
          const auto self=weak.lock();if(!self||self->closing||!self->hwnd)return S_OK;
          if(FAILED(error)||!env){self->fail(multitap::Tr(L"WebView2를 시작하지 못했어요.\nMicrosoft WebView2 Runtime을 설치하거나 업데이트해 주세요.", L"Couldn't start WebView2.\nInstall or update the Microsoft WebView2 Runtime."));return S_OK;}
          self->environment=env;
          ComPtr<ICoreWebView2Environment5> lifetime;
          if(SUCCEEDED(self->environment.As(&lifetime))){
            struct ProfileExit{ComPtr<ICoreWebView2Environment5> held;EventRegistrationToken token{};std::filesystem::path directory;};
            const auto cleanup=std::make_shared<ProfileExit>();cleanup->held=lifetime;cleanup->directory=self->folder;
            const auto registered=lifetime->add_BrowserProcessExited(Callback<ICoreWebView2BrowserProcessExitedEventHandler>([cleanup](ICoreWebView2Environment*,ICoreWebView2BrowserProcessExitedEventArgs*)->HRESULT{
              if(!cleanup->held)return S_OK;auto held=std::move(cleanup->held);held->remove_BrowserProcessExited(cleanup->token);
              // Keep the environment alive through its one-shot exit event;
              // best-effort file cleanup never blocks the application UI.
              try{std::thread([directory=cleanup->directory]{removeProfile(directory);}).detach();}catch(...){ }
              return S_OK;
            }).Get(),&cleanup->token);
            if(FAILED(registered))cleanup->held.Reset();
          }
          ComPtr<ICoreWebView2Environment10> privateEnv;ComPtr<ICoreWebView2ControllerOptions> controlOptions;
          if(FAILED(env->QueryInterface(IID_PPV_ARGS(&privateEnv)))||FAILED(privateEnv->CreateCoreWebView2ControllerOptions(&controlOptions))||FAILED(controlOptions->put_IsInPrivateModeEnabled(TRUE))){self->fail(multitap::Tr(L"WebView2 업데이트가 필요해요.\n창을 닫고 Runtime을 업데이트해 주세요.", L"WebView2 needs an update.\nClose the window and update the Runtime."));return S_OK;}
          // Per-window InPrivate profiles keep console cookies off disk and
          // prevent another account/VM window from inheriting this session.
          const auto leaf=self->folder.filename().wstring();const auto profile=leaf.substr(1,leaf.size()-2);controlOptions->put_ProfileName(profile.c_str());
          const auto created=privateEnv->CreateCoreWebView2ControllerWithOptions(self->hwnd,controlOptions.Get(),
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([weak](HRESULT hr,ICoreWebView2Controller* control)->HRESULT{
              const auto window=weak.lock();if(!window||window->closing||!window->hwnd){if(control)control->Close();return S_OK;}
              if(FAILED(hr)||!control){window->fail(multitap::Tr(L"Console 화면을 만들지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't create the Console screen.\nClose the window and connect again."));return S_OK;}
              window->controller=control;if(FAILED(control->get_CoreWebView2(&window->view))){window->fail(multitap::Tr(L"Console을 시작하지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't start Console.\nClose the window and connect again."));return S_OK;}
              window->configure();return S_OK;
            }).Get());
          if(FAILED(created))self->fail(multitap::Tr(L"Console 화면을 만들지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't create the Console screen.\nClose the window and connect again."));return S_OK;
        }).Get());
      if(FAILED(result))fail(multitap::Tr(L"WebView2를 시작하지 못했어요.\nMicrosoft WebView2 Runtime을 확인해 주세요.", L"Couldn't start WebView2.\nCheck the Microsoft WebView2 Runtime."));
    }
    void configure(){
      ComPtr<ICoreWebView2Settings> settings;if(FAILED(view->get_Settings(&settings))){fail(multitap::Tr(L"Console을 시작하지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't start Console.\nClose the window and connect again."));return;}
      HRESULT security=S_OK;const auto protect=[&](HRESULT hr){if(FAILED(hr)&&SUCCEEDED(security))security=hr;};
      protect(settings->put_IsScriptEnabled(TRUE));protect(settings->put_AreDevToolsEnabled(FALSE));protect(settings->put_AreDefaultContextMenusEnabled(FALSE));
      // Web messages carry only the fixed close strings, from the console page.
      protect(settings->put_AreHostObjectsAllowed(FALSE));protect(settings->put_IsWebMessageEnabled(TRUE));protect(settings->put_AreDefaultScriptDialogsEnabled(FALSE));protect(settings->put_IsStatusBarEnabled(FALSE));
      // UI hint only: expired embedded consoles return users to the app.
      ComPtr<ICoreWebView2Settings2> agentSettings;protect(settings.As(&agentSettings));
      if(agentSettings){LPWSTR raw=nullptr;const auto read=agentSettings->get_UserAgent(&raw);protect(read);
        if(SUCCEEDED(read)&&raw){const auto agent=std::wstring(raw)+L" Multitap-ConsoleWebView/1";protect(agentSettings->put_UserAgent(agent.c_str()));}
        else protect(E_FAIL);CoTaskMemFree(raw);
      }
      ComPtr<ICoreWebView2Settings3> shortcuts;protect(settings.As(&shortcuts));if(shortcuts)protect(shortcuts->put_AreBrowserAcceleratorKeysEnabled(FALSE));
      ComPtr<ICoreWebView2Settings4> passwords;protect(settings.As(&passwords));if(passwords){protect(passwords->put_IsPasswordAutosaveEnabled(FALSE));protect(passwords->put_IsGeneralAutofillEnabled(FALSE));}
      const std::weak_ptr<Window> weak=shared_from_this();EventRegistrationToken token{};
      // Media/keepalive events are excluded; report trusted source events only.
      protect(view->AddScriptToExecuteOnDocumentCreated(
        LR"JS((()=>{let last=-Infinity;for(const type of ['pointerdown','pointermove','pointerup','wheel','keydown','beforeinput'])addEventListener(type,e=>{if(!e.isTrusted)return;const now=performance.now();if(now-last<250)return;last=now;chrome.webview.postMessage('multitap-human-activity-v1')},true)})())JS",
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([weak](HRESULT hr,LPCWSTR)->HRESULT{
          if(FAILED(hr))if(const auto self=weak.lock())if(!self->closing)self->fail(multitap::Tr(L"Console을 시작하지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't start Console.\nClose the window and connect again."));return S_OK;
        }).Get()));
      protect(view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([weak](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT{
        const auto self=weak.lock();LPWSTR uri=nullptr;args->get_Uri(&uri);const auto url=takeUri(uri);
        if(!self||self->closing||!ConsoleWebViewNavigation(self->target,url,self->bootstrap))args->put_Cancel(TRUE);
        else self->closeFlow.Navigated();
        return S_OK;
      }).Get(),&token));
      protect(view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([weak](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT{
        // The page says ready while it loads, before NavigationCompleted.
        const auto self=weak.lock();if(!self||self->closing||!self->hwnd)return S_OK;
        // Only the console page itself, and only a plain string, is heard.
        LPWSTR source=nullptr;if(FAILED(args->get_Source(&source)))return S_OK;
        if(!ConsoleWebViewNavigation(self->target,takeUri(source),false))return S_OK;
        LPWSTR text=nullptr;const auto read=args->TryGetWebMessageAsString(&text);
        if(SUCCEEDED(read)&&text&&std::wstring(text)==L"multitap-human-activity-v1") {
          CoTaskMemFree(text);if(const auto owner=self->owner.lock())if(owner->human)owner->human();return S_OK;
        }
        const auto message=SUCCEEDED(read)&&text?ParseConsolePageMessage(text):ConsolePageMessage::Ignored;CoTaskMemFree(text);
        self->step(self->closeFlow.Message(message));return S_OK;
      }).Get(),&token));
      protect(view->add_ContainsFullScreenElementChanged(Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>([weak](ICoreWebView2* sender,IUnknown*)->HRESULT{
        const auto self=weak.lock();BOOL full=FALSE;
        if(self&&!self->closing&&SUCCEEDED(sender->get_ContainsFullScreenElement(&full)))self->setFullscreen(full!=FALSE);return S_OK;
      }).Get(),&token));
      protect(view->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT{args->put_Cancel(TRUE);return S_OK;}).Get(),&token));
      protect(view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT{args->put_Handled(TRUE);return S_OK;}).Get(),&token));
      protect(view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* args)->HRESULT{args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);return S_OK;}).Get(),&token));
      protect(view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([weak](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT{
        const auto self=weak.lock();if(!self||self->closing)return S_OK;BOOL success=FALSE;args->get_IsSuccess(&success);
        if(!success)self->fail(multitap::Tr(L"Console 화면을 열지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't open the Console screen.\nClose the window and connect again."));else self->bootstrap=false;return S_OK;
      }).Get(),&token));
      protect(view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([weak](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT{if(const auto self=weak.lock())self->fail(multitap::Tr(L"Console 화면이 멈췄어요.\n창을 닫고 다시 연결하세요.", L"Console stopped responding.\nClose the window and connect again."));return S_OK;}).Get(),&token));
      ComPtr<ICoreWebView2_4> downloads;protect(view.As(&downloads));if(downloads)protect(downloads->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2*,ICoreWebView2DownloadStartingEventArgs* args)->HRESULT{args->put_Cancel(TRUE);return S_OK;}).Get(),&token));
      ComPtr<ICoreWebView2_14> certificates;if(SUCCEEDED(view.As(&certificates)))protect(certificates->add_ServerCertificateErrorDetected(Callback<ICoreWebView2ServerCertificateErrorDetectedEventHandler>([](ICoreWebView2*,ICoreWebView2ServerCertificateErrorDetectedEventArgs* args)->HRESULT{args->put_Action(COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_CANCEL);return S_OK;}).Get(),&token));
      protect(view->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
      protect(view->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([weak](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args)->HRESULT{
        const auto self=weak.lock();if(!self||self->closing)return S_OK;ComPtr<ICoreWebView2WebResourceRequest> request;LPWSTR uri=nullptr;
        if(FAILED(args->get_Request(&request))||FAILED(request->get_Uri(&uri)))return E_FAIL;
        const auto url=takeUri(uri);COREWEBVIEW2_WEB_RESOURCE_CONTEXT context{};args->get_ResourceContext(&context);
        if(self->bootstrap&&url==self->target.attach&&context==COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT){
          const auto restored=RestoreConsoleBootstrapOrigin(request.Get(),self->target,context,self->attachOriginHandled);
          if(FAILED(restored)){
            ComPtr<ICoreWebView2WebResourceResponse> denied;if(SUCCEEDED(self->environment->CreateWebResourceResponse(nullptr,403,L"Forbidden",L"Cache-Control: no-store",&denied)))args->put_Response(denied.Get());
            self->fail(multitap::Tr(L"Console 인증을 시작하지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't start Console authentication.\nClose the window and connect again."));return S_OK;
          }
        }
        if(ConsoleWebViewResource(self->target,url)||(context==COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE&&(url.rfind("data:",0)==0||url.rfind("blob:",0)==0)))return S_OK;
        ComPtr<ICoreWebView2WebResourceResponse> denied;if(SUCCEEDED(self->environment->CreateWebResourceResponse(nullptr,403,L"Forbidden",L"Cache-Control: no-store",&denied)))args->put_Response(denied.Get());return S_OK;
      }).Get(),&token));
      if(FAILED(security)){fail(multitap::Tr(L"Console을 안전하게 열 수 없어요.\nWebView2 Runtime을 업데이트해 주세요.", L"Console can't be opened securely.\nUpdate the WebView2 Runtime."));return;}
      ComPtr<ICoreWebView2_13> profiles;if(SUCCEEDED(view.As(&profiles)))profiles->get_Profile(&profile);
      theme();size();controller->put_IsVisible(TRUE);
      if(FAILED(NavigateConsoleBootstrap(environment.Get(),view.Get(),target))){fail(multitap::Tr(L"Console 인증을 시작하지 못했어요.\n창을 닫고 다시 연결하세요.", L"Couldn't start Console authentication.\nClose the window and connect again."));return;}
      controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    void destroy(){
      if(closing)return;closing=true;clearCode(target.code);
      if(controller)controller->Close();view.Reset();controller.Reset();profile.Reset();environment.Reset();
    }
    // Shown until the page draws, and when WebView2 fails: the page's colours
    // and font instead of the system font on a fixed grey.
    void paint(HDC dc){
      RECT bounds{};GetClientRect(hwnd,&bounds);
      const auto brush=CreateSolidBrush(dark?RGB(14,18,22):RGB(238,241,244));FillRect(dc,&bounds,brush);DeleteObject(brush);
      const UINT dpi=GetDpiForWindow(hwnd);const int margin=MulDiv(32,dpi,96);
      const auto font=CreateFontW(-MulDiv(15,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Malgun Gothic");
      const auto previous=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,dark?RGB(228,233,236):RGB(26,32,37));
      bounds.left+=margin;bounds.top+=margin;bounds.right-=margin;DrawTextW(dc,status.c_str(),-1,&bounds,DT_LEFT|DT_TOP|DT_WORDBREAK);
      SelectObject(dc,previous);DeleteObject(font);
    }
    static LRESULT CALLBACK message(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
      Window* raw=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
      if(message==WM_NCCREATE){raw=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);raw->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(raw));}
      if(!raw)return DefWindowProcW(hwnd,message,wp,lp);const auto self=raw->shared_from_this();
      switch(message){
        case WM_SIZE:self->size();return 0;
        case WM_MOVE:if(self->controller)self->controller->NotifyParentWindowPositionChanged();return DefWindowProcW(hwnd,message,wp,lp);
        case WM_DPICHANGED:{const auto* suggested=reinterpret_cast<RECT*>(lp);if(suggested)SetWindowPos(hwnd,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
        case WM_SETFOCUS:if(self->controller)self->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);return 0;
        // X, Alt+F4, the system menu: the page asks in its own dialog when it
        // can; otherwise (or without an answer) the native question.
        case WM_CLOSE:if(!self->closing)self->step(self->closeFlow.Request());return 0;
        case WM_TIMER:if(wp!=closeAnswerTimer)return DefWindowProcW(hwnd,message,wp,lp);KillTimer(hwnd,wp);self->step(self->closeFlow.Unanswered());return 0;
        case closeConfirmed:self->closeNow();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT paint{};const auto dc=BeginPaint(hwnd,&paint);self->paint(dc);EndPaint(hwnd,&paint);return 0;}
        case WM_DESTROY:self->destroy();return 0;
        case WM_NCDESTROY:{SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);self->hwnd=nullptr;if(const auto owner=self->owner.lock()){owner->windows.erase(self->target.origin+'|'+self->target.desktop_id);if(owner->closed)owner->closed(self->target.origin,self->target.desktop_id,self->owner_id);}return DefWindowProcW(hwnd,message,wp,lp);}
        default:return DefWindowProcW(hwnd,message,wp,lp);
      }
    }
  };
  HWND owner;Closed closed;std::function<void()> human;
  std::map<std::string,std::shared_ptr<Window>> windows;
  Impl(HWND hwnd,Closed callback):owner(hwnd),closed(std::move(callback)){}
  void closeAll(){std::vector<std::shared_ptr<Window>> copy;for(const auto& pair:windows)copy.push_back(pair.second);for(const auto& window:copy)if(window->hwnd)DestroyWindow(window->hwnd);}
};
ConsoleWebViews::ConsoleWebViews(HWND owner,Closed closed):impl_(std::make_shared<Impl>(owner,std::move(closed))){}
ConsoleWebViews::~ConsoleWebViews(){CloseAll();impl_->closed={};}
std::string ConsoleWebViews::Open(ConsoleWebViewTarget target,bool dark){
  const auto key=target.origin+'|'+target.desktop_id;const auto existing=impl_->windows.find(key);
  if(existing!=impl_->windows.end()&&existing->second->hwnd){ShowWindow(existing->second->hwnd,SW_RESTORE);SetForegroundWindow(existing->second->hwnd);clearCode(target.code);return {};}
  LPWSTR version=nullptr;const auto available=GetAvailableCoreWebView2BrowserVersionString(nullptr,&version);const bool installed=SUCCEEDED(available)&&version;CoTaskMemFree(version);
  if(!installed){clearCode(target.code);return "console_webview_runtime_required";}
  WNDCLASSEXW type{sizeof(type)};type.lpfnWndProc=Impl::Window::message;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=windowClass;type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
  type.hIcon=LoadIconW(type.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));RegisterClassExW(&type);
  const auto window=std::make_shared<Impl::Window>();window->owner=impl_;window->dark=dark;window->owner_id=ConsoleWebViewOwnerId(target.code);
  if(window->owner_id.empty()){clearCode(target.code);return "console_webview_unavailable";}
  window->target=std::move(target);impl_->windows.emplace(key,window);
  MONITORINFO monitor{sizeof(monitor)};
  if(!GetMonitorInfoW(MonitorFromWindow(impl_->owner,MONITOR_DEFAULTTONEAREST),&monitor)){impl_->windows.erase(key);clearCode(window->target.code);return "console_webview_unavailable";}
  const int width=std::min(1100,int(monitor.rcWork.right-monitor.rcWork.left)),height=std::min(760,int(monitor.rcWork.bottom-monitor.rcWork.top));
  const int left=monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,top=monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2;
  const auto hwnd=CreateWindowExW(WS_EX_APPWINDOW,windowClass,L"Multitap · Console",WS_OVERLAPPEDWINDOW,left,top,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),window.get());
  if(!hwnd){impl_->windows.erase(key);clearCode(window->target.code);return "console_webview_unavailable";}
  window->theme();ShowWindow(hwnd,SW_SHOW);window->initialize();return {};
}
void ConsoleWebViews::SetDark(bool dark){
  if(!impl_)return;
  for(const auto& pair:impl_->windows)if(pair.second->dark!=dark){pair.second->dark=dark;pair.second->theme();}
}
void ConsoleWebViews::CloseAll(){if(impl_)impl_->closeAll();}
void ConsoleWebViews::HideAll(){if(impl_)for(const auto& pair:impl_->windows)if(pair.second->hwnd)ShowWindow(pair.second->hwnd,SW_HIDE);}
void ConsoleWebViews::SetHuman(std::function<void()> callback){if(impl_)impl_->human=std::move(callback);}
void ConsoleWebViews::Close(const std::string& origin,const std::string& desktop_id){
  if(!impl_)return;const auto found=impl_->windows.find(origin+'|'+desktop_id);
  if(found!=impl_->windows.end()&&found->second->hwnd)DestroyWindow(found->second->hwnd);
}
size_t ConsoleWebViews::Count()const{return impl_?impl_->windows.size():0;}
}
