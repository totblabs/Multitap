#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include "../../windows_client/windows/runner/window_close_guard.h"
namespace {
struct State { multitap::WindowCloseGuard guard; int requests=0; };
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE){state=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(state));}
  if(message==WM_CLOSE&&state&&state->guard.BlocksClose()){state->requests++;return 0;}
  return DefWindowProcW(window,message,wp,lp);
}
}
int main() {
  WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"MultitapCloseGuardQA";RegisterClassW(&type);
  State state;const HWND window=CreateWindowExW(0,type.lpszClassName,L"Multitap close guard QA",WS_OVERLAPPEDWINDOW,0,0,320,200,nullptr,nullptr,type.hInstance,&state);
  if(!window)return 1;
  bool passed=true;state.guard.Update(true,false);SendMessageW(window,WM_CLOSE,0,0);passed&=IsWindow(window)&&state.requests==1&&!state.guard.RequestClose();
  state.guard.Update(false,true);SendMessageW(window,WM_CLOSE,0,0);passed&=IsWindow(window)&&state.requests==2;
  passed&=state.guard.RequestClose();state.guard.Update(true,true);SendMessageW(window,WM_CLOSE,0,0);passed&=IsWindow(window)&&state.requests==3;
  state.guard.Update(false,true);passed&=state.guard.RequestClose();SendMessageW(window,WM_CLOSE,0,0);passed&=!IsWindow(window);
  State clean;const HWND idle=CreateWindowExW(0,type.lpszClassName,L"Multitap idle close QA",WS_OVERLAPPEDWINDOW,0,0,320,200,nullptr,nullptr,type.hInstance,&clean);
  if(!idle)return 1;SendMessageW(idle,WM_CLOSE,0,0);passed&=!IsWindow(idle)&&clean.requests==0;
  std::printf("{\"actualOwnWindowCloseCases\":5,\"busyCloseBlocked\":%s,\"dirtyCloseRequiresApproval\":%s,\"staleApprovalRevokedByNewWork\":%s,\"vmOperations\":0}\n",passed?"true":"false",passed?"true":"false",passed?"true":"false");
  return passed?0:1;
}
