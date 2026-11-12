#pragma once
#include "support.hpp"
#include <shellapi.h>
namespace host {
class HostTray {
 public:
  int run(bool selfTest=false) {
    selfTest_=selfTest;
    const auto mutexName=selfTest?L"Local\\MultitapHostTray.QA."+std::to_wstring(GetCurrentProcessId()):std::wstring(L"Local\\MultitapHostTray");
    mutex_=CreateMutexW(nullptr,FALSE,mutexName.c_str());
    if(!mutex_||GetLastError()==ERROR_ALREADY_EXISTS){if(mutex_)CloseHandle(mutex_);return selfTest?2:0;}
    WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"MultitapHostTray025";RegisterClassW(&type);
    window_=CreateWindowExW(WS_EX_TOOLWINDOW,type.lpszClassName,L"Multitap Host",WS_POPUP,0,0,0,0,nullptr,nullptr,type.hInstance,this);
    if(!window_){CloseHandle(mutex_);return 1;}
    restart_=RegisterWindowMessageW(L"TaskbarCreated");
    light_=lightTaskbar();icon_=loadMark(light_);
    data_.cbSize=sizeof(data_);data_.hWnd=window_;data_.uID=1;data_.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;data_.uCallbackMessage=WM_APP+11;data_.hIcon=icon_;
    refresh(false);add();SetTimer(window_,1,selfTest?250:10000,nullptr);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    Shell_NotifyIconW(NIM_DELETE,&data_);DestroyIcon(icon_);CloseHandle(mutex_);return result_;
  }
 private:
  HWND window_=nullptr;HANDLE mutex_=nullptr;HICON icon_=nullptr;NOTIFYICONDATAW data_{};UINT restart_=0;bool selfTest_=false,light_=false;int result_=0;
  // The notification area shows the bare mark in one colour, not the app tile: this executable's
  // icon resource 2 is amber for a dark taskbar, resource 3 the dark ink for a light one
  // (resources/host_tray.ico, host_tray_light.ico). The app icon, resource 1, is the fallback.
  // Windows keeps the taskbar's own light or dark choice in this value; missing means dark. Read only.
  static bool lightTaskbar(){DWORD value=0,size=sizeof(value);return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS&&value!=0;}
  static HICON loadMark(bool light){const auto module=GetModuleHandleW(nullptr);const int width=GetSystemMetrics(SM_CXSMICON),height=GetSystemMetrics(SM_CYSMICON);
    for(const int id:{light?3:2,1})if(const auto icon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(id),IMAGE_ICON,width,height,LR_DEFAULTCOLOR)))return icon;
    return CopyIcon(LoadIconW(nullptr,IDI_APPLICATION));}
  // A failed modify means the icon is gone: NIM_ADD failed at sign-in while the
  // notification area was not ready yet (no TaskbarCreated follows then), or
  // the shell dropped it. Add it again instead of staying hidden all session.
  // The same pass follows the taskbar when the user switches between light and dark.
  void refresh(bool notify=true){HICON old=nullptr;if(const bool light=lightTaskbar();light!=light_){light_=light;old=icon_;icon_=loadMark(light);data_.hIcon=icon_;}
    const auto label=ui_text::format(ui_text::Id::TrayTip,{serviceLabel()});wcsncpy_s(data_.szTip,label.c_str(),_TRUNCATE);if(notify&&!Shell_NotifyIconW(NIM_MODIFY,&data_))add();if(old)DestroyIcon(old);}
  void add(){if(!Shell_NotifyIconW(NIM_ADD,&data_)){result_=1;return;}data_.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&data_);}
  void settings(){const auto directory=std::filesystem::path(executablePath()).parent_path();const auto file=directory/L"MultitapHostSettings.exe";
    if(!std::filesystem::is_regular_file(file)){MessageBoxW(window_,ui_text::get(ui_text::Id::TraySettingsMissing),L"Multitap Host",MB_OK|MB_ICONERROR);return;}
    SHELLEXECUTEINFOW launch{sizeof(launch)};launch.hwnd=window_;launch.fMask=SEE_MASK_FLAG_NO_UI;launch.lpVerb=L"runas";launch.lpFile=file.c_str();launch.lpParameters=L"--host-settings";launch.lpDirectory=directory.c_str();launch.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&launch)&&GetLastError()!=ERROR_CANCELLED)MessageBoxW(window_,ui_text::get(ui_text::Id::TraySettingsFailed),L"Multitap Host",MB_OK|MB_ICONERROR);}
  void menu(){refresh();HMENU menu=CreatePopupMenu();const auto label=ui_text::format(ui_text::Id::TrayMenuStatus,{serviceLabel()});AppendMenuW(menu,MF_STRING|MF_GRAYED,0,label.c_str());AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,1,ui_text::get(ui_text::Id::TrayOpenSettings));AppendMenuW(menu,MF_STRING,2,ui_text::get(ui_text::Id::TrayRefresh));AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,3,ui_text::get(ui_text::Id::TrayClose));
    POINT point{};GetCursorPos(&point);SetForegroundWindow(window_);UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window_,nullptr);DestroyMenu(menu);PostMessageW(window_,WM_NULL,0,0);
    if(choice==1)settings();else if(choice==2)refresh();else if(choice==3&&MessageBoxW(window_,ui_text::get(ui_text::Id::TrayCloseBody),ui_text::get(ui_text::Id::TrayClose),MB_OKCANCEL|MB_ICONINFORMATION)==IDOK)DestroyWindow(window_);
  }
  LRESULT message(UINT message,WPARAM wp,LPARAM lp){
    if(restart_&&message==restart_){add();refresh();return 0;}
    if(message==WM_APP+11){const auto event=LOWORD(lp);if(event==WM_LBUTTONDBLCLK||event==NIN_KEYSELECT)settings();else if(event==WM_CONTEXTMENU||event==WM_RBUTTONUP)menu();return 0;}
    // Self-test: the icon disappears behind the tray's back; the periodic refresh must bring it back.
    if(message==WM_TIMER){if(selfTest_){Shell_NotifyIconW(NIM_DELETE,&data_);refresh();if(!Shell_NotifyIconW(NIM_MODIFY,&data_))result_=1;DestroyWindow(window_);}else refresh();return 0;}
    if(message==WM_DESTROY){KillTimer(window_,1);PostQuitMessage(0);return 0;}
    return DefWindowProcW(window_,message,wp,lp);
  }
  static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* tray=reinterpret_cast<HostTray*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){tray=static_cast<HostTray*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);tray->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(tray));}return tray?tray->message(message,wp,lp):DefWindowProcW(window,message,wp,lp);}
};
}
