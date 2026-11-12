#pragma once
#include "service.hpp"
#include "service_configuration_change.hpp"
#include <commctrl.h>
#include <shellapi.h>
namespace host {
inline constexpr UINT SettingsResult = WM_APP + 32;
inline void reloadHostService() { requireAdmin(); ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)); ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_USER_DEFINED_CONTROL)); SERVICE_STATUS status{}; winCheck(ControlService(service.value,hostServiceReloadControl,&status),"Reload host service"); }
inline bool requestHostCapabilitiesRefresh() noexcept {
  try { requireAdmin(); ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)); ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_USER_DEFINED_CONTROL)); SERVICE_STATUS status{}; return ControlService(service.value,hostServiceCapabilitiesControl,&status)!=FALSE; }
  catch (...) { return false; }
}
struct HostServiceSnapshot {
  bool presenceKnown=false, runningKnown=false, installed=false, running=false;
  DWORD state=0, error=0;
  const char* key()const {
    if(!presenceKnown||!runningKnown)return "unknown";
    if(!installed)return "not-installed";
    switch(state){case SERVICE_RUNNING:return "running";case SERVICE_STOPPED:return "stopped";case SERVICE_START_PENDING:return "starting";case SERVICE_STOP_PENDING:return "stopping";case SERVICE_PAUSED:return "paused";case SERVICE_PAUSE_PENDING:return "pausing";case SERVICE_CONTINUE_PENDING:return "continuing";default:return "unknown";}
  }
};
inline HostServiceSnapshot readHostServiceSnapshot(const wchar_t* name=L"MultitapHost") {
  HostServiceSnapshot value;const auto managerHandle=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
  if(!managerHandle){value.error=GetLastError();return value;}ServiceHandle manager(managerHandle);
  const auto serviceHandle=OpenServiceW(manager.value,name,SERVICE_QUERY_STATUS);
  if(!serviceHandle){value.error=GetLastError();if(value.error==ERROR_SERVICE_DOES_NOT_EXIST){value.presenceKnown=value.runningKnown=true;value.error=0;}return value;}ServiceHandle service(serviceHandle);
  value.presenceKnown=value.installed=true;SERVICE_STATUS_PROCESS status{};DWORD bytes=0;
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes)){value.error=GetLastError();return value;}
  value.runningKnown=true;value.state=status.dwCurrentState;value.running=status.dwCurrentState==SERVICE_RUNNING;return value;
}
// The service state in words: the tray shows it in the person's language;
// --diagnostics keeps the Korean words it always printed (Language::korean).
inline std::wstring serviceLabel(const HostServiceSnapshot& value,ui_text::Language language) {
  using ui_text::Id;const auto say=[language](Id id){return std::wstring(ui_text::get(id,language));};
  if(!value.presenceKnown||!value.runningKnown)return say(Id::ServiceUnknown);
  if(!value.installed)return say(Id::ServiceNotInstalled);
  switch(value.state){case SERVICE_RUNNING:return say(Id::ServiceRunning);case SERVICE_STOPPED:return say(Id::ServiceStopped);case SERVICE_START_PENDING:return say(Id::ServiceStarting);case SERVICE_STOP_PENDING:return say(Id::ServiceStopping);case SERVICE_PAUSED:return say(Id::ServicePaused);case SERVICE_PAUSE_PENDING:case SERVICE_CONTINUE_PENDING:return say(Id::ServiceChanging);default:return say(Id::ServiceUnknown);}
}
inline std::wstring serviceLabel(const HostServiceSnapshot& value){return serviceLabel(value,multitap::ui::processLanguage());}
inline std::wstring serviceLabel(){return serviceLabel(readHostServiceSnapshot());}
class SettingsUi {
 public:
  int run(const std::wstring& testOrigin = L"", double layoutScale = 0) {
    testOrigin_=testOrigin;
    scale_=layoutScale>0?layoutScale:double(GetDpiForSystem())/96.0;
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    WNDCLASSW type{}; type.lpfnWndProc=procedure;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"MultitapHostSettings020";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hIcon=LoadIconW(type.hInstance,MAKEINTRESOURCEW(1));RegisterClassW(&type);
    RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);const int height=std::min(int(840*scale_),int(work.bottom-work.top-35));window_=CreateWindowExW(0,type.lpszClassName,L"Multitap Host 설정 · " MULTITAP_HOST_VERSION,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VSCROLL,CW_USEDEFAULT,CW_USEDEFAULT,int(680*scale_),height,nullptr,nullptr,type.hInstance,this);winCheck(window_!=nullptr,"Create host settings");ShowWindow(window_,SW_SHOW);
    if(!testOrigin_.empty()){SetWindowTextW(origin_,testOrigin_.c_str());command(Test);SetTimer(window_,77,50,nullptr);}
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window_,&message)){TranslateMessage(&message);DispatchMessageW(&message);}} if(worker_.joinable())worker_.join();return testOrigin_.empty()||selfTestPassed_?0:1;
  }
 private:
  enum {Test=101,Enroll=102,Admin=103,Refresh=105,Start=106,Stop=107,Display=108};
  HWND window_=nullptr,origin_=nullptr,code_=nullptr,test_=nullptr,enroll_=nullptr,status_=nullptr,system_=nullptr,display_=nullptr;HFONT font_=nullptr,title_=nullptr;HBRUSH background_=nullptr;std::thread worker_;std::atomic<bool> busy_{false};std::wstring tested_,testOrigin_;bool selfTestPassed_=false;double scale_=1;int scroll_=0;
  struct Result { bool ok=false;int action=0;std::wstring address,text; };
  static std::wstring text(HWND control){int count=GetWindowTextLengthW(control);std::wstring value(count+1,0);GetWindowTextW(control,value.data(),count+1);value.resize(count);return value;}
  HWND add(const wchar_t* kind,const wchar_t* label,DWORD style,int x,int y,int width,int height,int id=0){auto handle=CreateWindowExW(std::wstring(kind)==L"EDIT"?WS_EX_CLIENTEDGE:0,kind,label,WS_CHILD|WS_VISIBLE|style,int(x*scale_),int(y*scale_),int(width*scale_),int(height*scale_),window_,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(handle,WM_SETFONT,WPARAM(font_),TRUE);return handle;}
  void show(const std::wstring& value){SetWindowTextW(status_,value.c_str());}
  void update(){EnableWindow(origin_,!busy_);EnableWindow(code_,!busy_);EnableWindow(test_,!busy_);EnableWindow(enroll_,!busy_&&!tested_.empty()&&tested_==text(origin_));}
  void elevate(){SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS;info.lpVerb=L"runas";const auto executable=executablePath();info.lpFile=executable.c_str();info.lpParameters=L"--configure";info.nShow=SW_SHOWNORMAL;if(ShellExecuteExW(&info)){if(info.hProcess)CloseHandle(info.hProcess);DestroyWindow(window_);}else show(L"관리자 승인이 취소됐습니다.");}
  void refresh(){SetWindowTextW(system_,serviceLabel().c_str());const auto display=virtualDisplayStatus();const std::wstring label=L"가상 모니터: "+std::to_wstring(display.monitors.size())+L"/2 · "+(display.ready()?L"현재 세션에서 준비됨":L"설치·콘솔 세션 확인 필요");SetWindowTextW(display_,label.c_str());}
  template<class Work> void begin(int action,std::wstring address,Work work){if(busy_.exchange(true))return;if(worker_.joinable())worker_.join();update();worker_=std::thread([this,action,address=std::move(address),work=std::move(work)]()mutable{auto result=std::make_unique<Result>();result->action=action;result->address=address;try{result->text=work();result->ok=true;}catch(const std::exception& error){result->text=wide(error.what());}if(IsWindow(window_))PostMessageW(window_,SettingsResult,0,LPARAM(result.release()));});}
  void command(int id){
    if(busy_)return;if(id==Admin){elevate();return;}
    if(id==Test){const auto address=text(origin_);tested_.clear();show(L"HTTPS·인증서·서버 제품 정보를 확인하는 중…");begin(Test,address,[address]{return L"연결 확인 완료 · Multitap Broker "+wide(testBroker(address));});}
    if(id==Enroll){if(!elevated()){elevate();return;}const auto address=text(origin_);if(address!=tested_||tested_.empty()){show(L"현재 주소의 연결 테스트를 먼저 통과해주세요.");return;}std::string code=utf8(text(code_));SetWindowTextW(code_,L"");show(L"VM 등록 및 서비스 연결을 적용하는 중…");begin(Enroll,address,[address,code=std::move(code)]()mutable{testBroker(address);auto credentials=enroll(address,code);SecureZeroMemory(code.data(),code.size());savePublicEnrollment(credentials);reloadHostService();return L"VM 등록 완료 · 서비스가 서버에 연결합니다.";});}
    if(id==Refresh){refresh();return;}
    if(id==Display){if(!elevated()){elevate();return;}show(L"가상 모니터 두 개를 설정하는 중…");begin(Display,L"",[]{const auto display=configureVirtualDisplays();return display.ready()?L"가상 화면 준비됨 · 상태에서 실제 모니터 수를 확인하세요.":L"가상 화면 설정을 확인해주세요. Proxmox 콘솔에서 이 설정 앱을 열어 가상 화면 설정을 실행하거나 Windows 재시작 후 다시 확인하세요.";});return;}
    if(id==Start||id==Stop){if(!elevated()){elevate();return;}try{serviceControl(L"MultitapHost",id==Stop?SERVICE_CONTROL_STOP:0);show(id==Stop?L"호스트 서비스 중지 요청됨":L"호스트 서비스 시작 요청됨");refresh();}catch(const std::exception& error){show(wide(error.what()));}return;}
  }
  LRESULT message(UINT message,WPARAM wp,LPARAM lp){switch(message){case WM_CREATE:{font_=CreateFontW(-int(16*scale_),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");title_=CreateFontW(-int(27*scale_),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");background_=CreateSolidBrush(RGB(245,247,251));auto heading=add(L"STATIC",L"Multitap Host",0,28,24,600,38);SendMessageW(heading,WM_SETFONT,WPARAM(title_),TRUE);add(L"STATIC",L"Windows 서비스 · 스트리밍 연결을 설정합니다.",0,30,72,605,26);
      add(L"BUTTON",elevated()?L"관리자 권한으로 실행 중":L"관리자 권한으로 설정",WS_TABSTOP|BS_PUSHBUTTON,30,110,290,34,Admin);add(L"BUTTON",L"상태 새로고침",WS_TABSTOP,334,110,290,34,Refresh);
      add(L"STATIC",L"클라우드 브로커 주소 (HTTPS)",0,30,165,590,23);origin_=add(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,30,193,590,34);SendMessageW(origin_,EM_SETLIMITTEXT,200,0);SendMessageW(origin_,EM_SETCUEBANNER,TRUE,LPARAM(L"https://vdi.example.com"));test_=add(L"BUTTON",L"연결 테스트",WS_TABSTOP,30,239,590,36,Test);
      add(L"STATIC",L"관리자 웹에서 이 VM에 발급한 일회용 등록 코드",0,30,291,590,23);code_=add(L"EDIT",L"",WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,30,319,590,34);SendMessageW(code_,EM_SETLIMITTEXT,128,0);enroll_=add(L"BUTTON",L"VM 등록 · 서비스 연결",WS_TABSTOP|BS_DEFPUSHBUTTON,30,365,590,38,Enroll);
      display_=add(L"STATIC",L"가상 모니터 확인 중",0,30,421,420,42);add(L"BUTTON",L"가상 화면 설정",WS_TABSTOP,463,423,157,34,Display);
      system_=add(L"STATIC",L"",0,30,599,590,48);add(L"BUTTON",L"서비스 시작",WS_TABSTOP,30,660,286,32,Start);add(L"BUTTON",L"서비스 중지",WS_TABSTOP,334,660,286,32,Stop);status_=add(L"STATIC",L"주소를 테스트한 뒤 등록하세요. 서비스가 로그인 전 서버 연결을 담당하며, 스트리밍은 로그인된 세션과 준비된 가상 화면이 필요합니다.",0,30,708,590,70);
      const auto config=readPublicConfig();SetWindowTextW(origin_,wide(config.get("origin").string()).c_str());refresh();update();return 0;}
    case WM_COMMAND:if(reinterpret_cast<HWND>(lp)==origin_&&HIWORD(wp)==EN_CHANGE){tested_.clear();update();}else command(LOWORD(wp));return 0;
    case SettingsResult:{std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp));if(worker_.joinable())worker_.join();busy_=false;if(result->action==Test&&result->ok&&text(origin_)==result->address)tested_=result->address;show(result->text);refresh();update();return 0;}
    case WM_SIZE:{RECT rect{};GetClientRect(window_,&rect);SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,int(805*scale_),UINT(rect.bottom),scroll_,0};SetScrollInfo(window_,SB_VERT,&info,TRUE);return 0;}
    case WM_VSCROLL:{SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window_,SB_VERT,&info);int next=scroll_;switch(LOWORD(wp)){case SB_LINEUP:next-=int(25*scale_);break;case SB_LINEDOWN:next+=int(25*scale_);break;case SB_PAGEUP:next-=int(info.nPage);break;case SB_PAGEDOWN:next+=int(info.nPage);break;case SB_THUMBTRACK:next=info.nTrackPos;break;}next=std::clamp(next,0,std::max(0,info.nMax-int(info.nPage)+1));SetScrollPos(window_,SB_VERT,next,TRUE);ScrollWindowEx(window_,0,scroll_-next,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);scroll_=next;return 0;}
    case WM_MOUSEWHEEL:SendMessageW(window_,WM_VSCROLL,GET_WHEEL_DELTA_WPARAM(wp)>0?SB_LINEUP:SB_LINEDOWN,0);return 0;
    case WM_TIMER:if(wp==77&&!busy_){KillTimer(window_,77);const bool accepted=tested_==testOrigin_&&IsWindowEnabled(enroll_);SetWindowTextW(origin_,(testOrigin_+L"1").c_str());bool layout=true;RECT client{};GetClientRect(window_,&client);for(HWND child=GetWindow(window_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){RECT rect{};GetWindowRect(child,&rect);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&rect),2);if(rect.right>client.right+1||rect.left<0||rect.bottom>int(805*scale_)+1)layout=false;}selfTestPassed_=accepted&&tested_.empty()&&!IsWindowEnabled(enroll_)&&layout;DestroyWindow(window_);return 0;}break;
    case WM_CTLCOLORSTATIC:SetBkColor(reinterpret_cast<HDC>(wp),RGB(245,247,251));SetTextColor(reinterpret_cast<HDC>(wp),RGB(32,48,73));return LRESULT(background_);
    case WM_ERASEBKGND:{RECT rect{};GetClientRect(window_,&rect);FillRect(reinterpret_cast<HDC>(wp),&rect,background_);return 1;}
    case WM_CLOSE:if(busy_){show(L"설정 작업이 끝난 뒤 창을 닫아주세요.");return 0;}DestroyWindow(window_);return 0;
    case WM_DESTROY:DeleteObject(font_);DeleteObject(title_);DeleteObject(background_);PostQuitMessage(0);return 0;}return DefWindowProcW(window_,message,wp,lp);}
  static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* ui=reinterpret_cast<SettingsUi*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){ui=static_cast<SettingsUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);ui->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(ui));}return ui?ui->message(message,wp,lp):DefWindowProcW(window,message,wp,lp);}
};
}
