#pragma once
// Included by setup_main.cpp after its shared native painting and roots
// (inside its unnamed namespace; setup_main.cpp includes setup_uninstall_reason.hpp itself).
// All product removal happens in a verified protected child, never the UI QA.
class RemovalWindow {
 public:
  int run(bool worker,std::function<void()> action={},bool preview=false){
    worker_=worker;action_=std::move(action);preview_=preview;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);scale_=double(GetDpiForSystem())/96.;
    Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&graphics_,&input,nullptr)!=Gdiplus::Ok)return 1;
    WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"MultitapNativeRemoval";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hIcon=LoadIconW(type.hInstance,MAKEINTRESOURCEW(1));RegisterClassW(&type);
    RECT frame{0,0,px(540),px(272)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,UINT(scale_*96));
    const auto product=tx(S::RemoveTitleClient);
    window_=CreateWindowExW(0,type.lpszClassName,product,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,frame.right-frame.left,frame.bottom-frame.top,nullptr,nullptr,type.hInstance,this);
    if(!window_){Gdiplus::GdiplusShutdown(graphics_);return 1;}darkTitleBar(window_);ShowWindow(window_,SW_SHOW);SetFocus(cancel_);if(worker_)SetTimer(window_,1,10,nullptr);
    MSG message{};while(IsWindow(window_)){const BOOL next=GetMessageW(&message,nullptr,0,0);if(next<=0)break;if(!IsDialogMessageW(window_,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(thread_.joinable())thread_.join();if(IsWindow(window_))DestroyWindow(window_);if(font_)DeleteObject(font_);Gdiplus::GdiplusShutdown(graphics_);return failed_?1:0;
  }
 private:
  HWND window_=nullptr,accept_=nullptr,cancel_=nullptr;HFONT font_=nullptr;ULONG_PTR graphics_=0;double scale_=1;
  bool worker_=false,finished_=false,failed_=false,preview_=false;std::string code_;std::function<void()> action_;std::thread thread_;
  int px(int value)const{return int(value*scale_);}
  void layout(){if(font_)DeleteObject(font_);font_=CreateFontW(-px(13),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,uiFamily());for(auto button:{accept_,cancel_})SendMessageW(button,WM_SETFONT,WPARAM(font_),TRUE);RECT r{};GetClientRect(window_,&r);MoveWindow(cancel_,r.right-px(294),r.bottom-px(62),px(112),px(38),TRUE);MoveWindow(accept_,r.right-px(170),r.bottom-px(62),px(142),px(38),TRUE);ShowWindow(accept_,worker_?SW_HIDE:SW_SHOW);ShowWindow(cancel_,worker_&&!finished_?SW_HIDE:SW_SHOW);SetWindowTextW(cancel_,tx(finished_?S::Close:S::Cancel));InvalidateRect(window_,nullptr,TRUE);}
  void paint(HDC dc){RECT r{};GetClientRect(window_,&r);Painter p(r.right,r.bottom,scale_,Canvas);const double width=r.right/scale_;
    // The heading names the product beside its badge (no separate product
    // label above it: the title bar already says what is being removed).
    drawBadge(p,28,28,38,failed_?Badge::Failure:finished_?Badge::Success:Badge::Question);
    p.text(tx(finished_?(failed_?S::RemoveFailedHeading:S::RemovedClient):worker_?S::RemovingClient:S::RemoveAskClient),80,28,width-108,38,20,Ink,Bold|Single|CenterV);
    // Which sentence a failure code gets is decided, and tested, in setup_uninstall_reason.hpp.
    const wchar_t* failure=multitap::installation::removalFailureText(multitap::installation::removalFailure(code_));
    const wchar_t* body=finished_?(failed_?failure:tx(S::RemovedBody)):tx(worker_?S::RemovingBody:S::RemoveAskBodyClient);
    p.text(body,28,90,width-56,48,13,Body,0,22);
    if(failed_){const auto wide=multitap::payload::widen(code_).value_or(L"uninstall_failed");p.text(txf(S::RemoveCode,{wide}),28,150,width-56,24,12,Danger,Single|CenterV);}
    else p.text(tx(S::RemoveUnchanged),28,150,width-56,24,12,Muted,Single|CenterV);
    p.present(dc);
  }
  void start(){EnableWindow(accept_,FALSE);try{if(preview_)return;action_();DestroyWindow(window_);}catch(const multitap::payload::Failure& failure){code_=failure.code;failed_=finished_=true;worker_=true;layout();}catch(...){code_="uninstall_failed";failed_=finished_=true;worker_=true;layout();}}
  LRESULT message(UINT message,WPARAM wp,LPARAM lp){switch(message){
    case WM_CREATE:accept_=CreateWindowExW(0,L"BUTTON",tx(S::RemoveAction),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window_,reinterpret_cast<HMENU>(IDOK),GetModuleHandleW(nullptr),nullptr);cancel_=CreateWindowExW(0,L"BUTTON",tx(S::Cancel),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window_,reinterpret_cast<HMENU>(IDCANCEL),GetModuleHandleW(nullptr),nullptr);layout();return 0;
    case DM_GETDEFID:return MAKELONG(IDCANCEL,DC_HASDEFID);
    case WM_COMMAND:if(LOWORD(wp)==IDOK&&!worker_){start();return 0;}if(LOWORD(wp)==IDCANCEL&&(!worker_||finished_)){DestroyWindow(window_);return 0;}break;
    case WM_TIMER:if(wp==1){KillTimer(window_,1);if(preview_){finished_=true;layout();return 0;}thread_=std::thread([this]{try{action_();}catch(const multitap::payload::Failure& failure){code_=failure.code;}catch(...){code_="uninstall_failed";}PostMessageW(window_,WM_APP+31,0,0);});}return 0;
    case WM_APP+31:if(thread_.joinable())thread_.join();finished_=true;failed_=!code_.empty();layout();SetFocus(cancel_);return 0;
    case WM_CLOSE:if(!worker_||finished_)DestroyWindow(window_);return 0;
    case WM_SIZE:if(accept_)layout();return 0;
    case WM_DPICHANGED:{const auto* r=reinterpret_cast<RECT*>(lp);if(!r||HIWORD(wp)<48)return 0;scale_=double(HIWORD(wp))/96.;SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;}
    case WM_PAINT:{PAINTSTRUCT ps{};const auto dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
    case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
    case WM_DRAWITEM:{const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);drawButton(*item,scale_,item->CtlID==IDOK?ButtonStyle::Primary:ButtonStyle::Secondary,Canvas);return TRUE;}
    case WM_ERASEBKGND:return 1;
  }return DefWindowProcW(window_,message,wp,lp);}
  static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<RemovalWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<RemovalWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(self));}return self?self->message(message,wp,lp):DefWindowProcW(window,message,wp,lp);}
};
