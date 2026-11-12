#include "remote_track_window.h"
#include "video_bounds.h"
#include <new>
namespace multitap {
namespace {
constexpr wchar_t className[]=L"Multitap.TrackWindow.v1";
class DpiScope {
 public:
  DpiScope():previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)){}
  ~DpiScope(){if(previous_)SetThreadDpiAwarenessContext(previous_);}
  bool valid()const{return previous_!=nullptr;}
 private:DPI_AWARENESS_CONTEXT previous_;
};
}
struct RemoteTrackWindow::State {
  Binding binding;Callback callback;HWND window=nullptr;DWORD thread=0;
  bool opening=true,closed=false,closing=false,busy=false;
  std::shared_ptr<RemoteTrackPresenter> presenter;
  RemoteTrackFrame last;
};
HWND RemoteTrackWindow::window()const{return state_&&!state_->closed&&IsWindow(state_->window)?state_->window:nullptr;}
int64_t RemoteTrackWindow::lease()const{return window()?state_->binding.lease:0;}
RemoteTrackPresenter::Stats RemoteTrackWindow::stats()const{return state_&&state_->presenter?state_->presenter->stats():RemoteTrackPresenter::Stats{};}
void RemoteTrackWindow::Emit(const std::shared_ptr<State>& state,const char* type,HRESULT error)noexcept{
  if(state->opening||state->closed)return;
  try{const auto callback=state->callback;if(!callback)return;RECT client{};GetClientRect(state->window,&client);const auto b=state->binding;callback({b.lease,b.track,b.topology,b.epoch,type,client.right-client.left,client.bottom-client.top,error});}catch(...){ }
}
HRESULT RemoteTrackWindow::Draw(const std::shared_ptr<State>& state,const RemoteTrackFrame& frame){
  if(state->closed||!IsWindow(state->window))return E_ABORT;
  if(state->thread!=GetCurrentThreadId())return E_ACCESSDENIED;
  const auto b=state->binding;const int64_t width=int64_t(b.bounds.right)-b.bounds.left,height=int64_t(b.bounds.bottom)-b.bounds.top;
  if(frame.track!=b.track||frame.topology!=b.topology||frame.epoch!=b.epoch)return E_ABORT;
  if(state->last.serial>frame.serial)return S_FALSE;
  if(!frame.serial||!frame.pixels||frame.pixels->width!=width||frame.pixels->height!=height)return E_INVALIDARG;
  if(state->busy||!state->presenter)return E_PENDING;
  const auto presenter=state->presenter;state->busy=true;HRESULT hr=E_FAIL;
  try{hr=presenter->Render(*frame.pixels);}catch(...){hr=E_FAIL;}
  state->busy=false;if(state->closed)return E_ABORT;
  if(hr==S_OK||hr==DXGI_STATUS_OCCLUDED||hr==DXGI_ERROR_WAS_STILL_DRAWING)state->last=frame;
  if(FAILED(hr)&&hr!=DXGI_ERROR_WAS_STILL_DRAWING)Emit(state,"error",hr);
  return hr;
}
HRESULT RemoteTrackWindow::Open(HWND owner,const Binding& b,Callback callback,bool visible,bool forceWarp)try{
  DWORD pid=0;const auto thread=GetWindowThreadProcessId(owner,&pid);const auto width=int64_t(b.bounds.right)-b.bounds.left,height=int64_t(b.bounds.bottom)-b.bounds.top;
  if(!owner||!IsWindow(owner)||pid!=GetCurrentProcessId()||thread!=GetCurrentThreadId()||b.lease<1||b.track>1||!b.topology||!b.epoch||width<2||height<2||uint64_t(width)>UINT32_MAX||uint64_t(height)>UINT32_MAX||!ValidVideoSize(uint32_t(width),uint32_t(height)))return E_INVALIDARG;
  if(b.lease<=highest_)return E_ABORT;
  DpiScope dpi;if(!dpi.valid())return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  if(b.fullscreen){MONITORINFO monitor{sizeof(monitor)};const auto display=MonitorFromRect(&b.bounds,MONITOR_DEFAULTTONULL);if(!display||!GetMonitorInfoW(display,&monitor)||!EqualRect(&monitor.rcMonitor,&b.bounds))return E_INVALIDARG;}
  WNDCLASSEXW type{sizeof(type)};type.lpfnWndProc=Procedure;type.hInstance=GetModuleHandleW(nullptr);type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=className;
  if(!RegisterClassExW(&type)){const auto error=GetLastError();WNDCLASSEXW existing{sizeof(existing)};if(error!=ERROR_CLASS_ALREADY_EXISTS||!GetClassInfoExW(type.hInstance,className,&existing)||existing.lpfnWndProc!=Procedure)return HRESULT_FROM_WIN32(error?error:ERROR_INVALID_DATA);}
  highest_=b.lease;Close();auto state=std::make_shared<State>();state->binding=b;state->callback=std::move(callback);state->thread=GetCurrentThreadId();
  const HWND created=CreateWindowExW(WS_EX_TOOLWINDOW,className,L"Multitap",WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,b.bounds.left,b.bounds.top,int(width),int(height),owner,nullptr,type.hInstance,&state);
  if(!created)return HRESULT_FROM_WIN32(GetLastError());state_=state;
  const auto presenter=std::make_shared<RemoteTrackPresenter>();const HRESULT hr=presenter->Initialize(created,forceWarp);
  if(FAILED(hr)){Close();return hr;}if(state->closed||!IsWindow(created))return E_ABORT;
  state->presenter=presenter;state->opening=false;
  if(visible)ShowWindow(created,SW_SHOWNOACTIVATE);return S_OK;
}catch(const std::bad_alloc&){Close();return E_OUTOFMEMORY;}catch(...){Close();return E_FAIL;}
HRESULT RemoteTrackWindow::Render(int64_t requested,const RemoteTrackFrame& frame){const auto state=state_;return !state||requested!=state->binding.lease?E_ABORT:Draw(state,frame);}
HRESULT RemoteTrackWindow::Close(int64_t requested){if(!state_||requested!=state_->binding.lease)return S_FALSE;if(state_->thread!=GetCurrentThreadId())return E_ACCESSDENIED;Close();return S_OK;}
void RemoteTrackWindow::Close(){const auto state=state_;if(!state||state->thread!=GetCurrentThreadId())return;state->closed=true;state->callback={};state->last={};state->presenter.reset();if(IsWindow(state->window))DestroyWindow(state->window);state_.reset();}
LRESULT CALLBACK RemoteTrackWindow::Procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)noexcept{
  auto* cookie=reinterpret_cast<std::shared_ptr<State>*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE){try{const auto* create=reinterpret_cast<CREATESTRUCTW*>(lparam);auto owned=std::make_unique<std::shared_ptr<State>>(*static_cast<std::shared_ptr<State>*>(create->lpCreateParams));SetLastError(0);if(!SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(owned.get()))&&GetLastError())return FALSE;cookie=owned.release();(*cookie)->window=window;}catch(...){return FALSE;}}
  if(!cookie)return DefWindowProcW(window,message,wparam,lparam);const auto state=*cookie;
  switch(message){
    case WM_CLOSE:if(!state->closed&&!state->closing){state->closing=true;Emit(state,"closed");if(!state->closed&&state->window==window){state->closed=true;state->presenter.reset();state->last={};DestroyWindow(window);}}return 0;
    case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);state->closed=true;state->window=nullptr;state->presenter.reset();state->last={};delete cookie;return DefWindowProcW(window,message,wparam,lparam);
    case WM_SIZE:Emit(state,"viewport");return 0;
    case WM_SETFOCUS:case WM_KILLFOCUS:Emit(state,"focus");break;
    case WM_DPICHANGED:{RECT bounds=*reinterpret_cast<const RECT*>(lparam);if(state->binding.fullscreen){MONITORINFO monitor{sizeof(monitor)};if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))bounds=monitor.rcMonitor;}SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOACTIVATE|SWP_NOZORDER);return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT paint{};BeginPaint(window,&paint);if(!state->last.pixels)FillRect(paint.hdc,&paint.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));EndPaint(window,&paint);if(!state->busy&&state->last.pixels){const auto frame=state->last;try{Draw(state,frame);}catch(...){}}return 0;}
  }
  return DefWindowProcW(window,message,wparam,lparam);
}
}
