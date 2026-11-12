#include "window_geometry.hpp"
#include "win32_window.h"
#include <flutter_windows.h>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
int checks=0;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);++checks;}
bool contained(const RECT& outer,const RECT& inner){return inner.right>inner.left&&inner.bottom>inner.top&&inner.left>=outer.left&&inner.top>=outer.top&&inner.right<=outer.right&&inner.bottom<=outer.bottom;}
void simulatedFrames(){
  using namespace multitap;
  const std::array<RECT,7> work{{{0,0,1366,728},{0,0,1280,680},{0,0,800,560},{-1920,-1080,0,-40},{2560,0,4480,1040},{0,0,3840,2080},{-1,-1,1,1}}};
  const std::array<UINT,8> dpis{0,96,120,144,192,240,384,480};
  for(const auto& area:work)for(const auto dpi:dpis)for(const bool host:{false,true}){
    const auto frame=initialWindowFrame(area,dpi,60,50,host?696:1440,host?800:900);
    const auto minimum=minimumWindowTrackSize(area,dpi,host?600:760,host?520:560);
    require(frame&&contained(area,*frame),"Initial window escaped its work area at a supported DPI");
    require(minimum&&minimum->cx>0&&minimum->cy>0&&minimum->cx<=frame->right-frame->left&&minimum->cy<=frame->bottom-frame->top,"Minimum tracking size exceeded the fitted initial frame");
  }
  const auto high=initialWindowFrame({LONG_MIN,LONG_MIN,LONG_MAX,LONG_MAX},UINT_MAX,LONG_MIN,LONG_MAX,UINT_MAX,UINT_MAX);
  require(high&&contained({LONG_MIN,LONG_MIN,LONG_MAX,LONG_MAX},*high)&&int64_t(high->right)-high->left<=LONG_MAX,"Extreme inputs overflowed signed window coordinates");
  require(!initialWindowFrame({0,0,0,600},96,60,50,1440,900)&&!minimumWindowTrackSize({0,600,800,0},96,760,560),"Empty or reversed work area was accepted");
  const auto standard=initialWindowFrame({0,0,1920,1040},96,60,50,1440,900);
  require(standard&&standard->left==60&&standard->top==50&&standard->right-standard->left==1440&&standard->bottom-standard->top==900,"A fitting ordinary desktop changed size or position");
  // Session windows: centred over the launcher, inside the launcher's monitor.
  const auto centered=centeredWindowFrame({100,50,1540,950},{0,0,1920,1040},1280,800);
  require(centered&&centered->left==180&&centered->top==100&&centered->right==1460&&centered->bottom==900,"Session window was not centred over the launcher");
  const auto edge=centeredWindowFrame({-200,700,600,1100},{0,0,1920,1040},1280,800);
  require(edge&&edge->left==0&&edge->bottom==1040&&contained({0,0,1920,1040},*edge),"Centred session window escaped the work area");
  const auto large=centeredWindowFrame({2560,0,4480,1040},{2560,0,4480,1040},4000,3000);
  require(large&&large->left==2560&&large->top==0&&large->right==4480&&large->bottom==1040,"Oversized session window was not fitted to the launcher's monitor");
  require(!centeredWindowFrame({0,0,10,10},{0,0,0,0},1280,800)&&!centeredWindowFrame({0,0,10,10},{0,0,1920,1040},0,800),"Empty work area or size was accepted");
}
int actualWindows(){
  int windows=0;
  const auto monitor=MonitorFromPoint(POINT{60,50},MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  require(GetMonitorInfoW(monitor,&monitorInfo)!=FALSE,"Actual monitor work area was unavailable");
  const UINT dpi=FlutterDesktopGetDpiForMonitor(monitor);
  for(const bool host:{false,true}){
    Win32Window window;window.SetMinimumSize(Win32Window::Size(host?600:760,host?520:560));
    require(window.Create(L"Multitap geometry QA",{60,50},{host?696u:1440u,host?800u:900u}),"Actual production Win32Window creation failed");
    ++windows;RECT frame{};
    require(GetWindowRect(window.GetHandle(),&frame)!=FALSE&&contained(monitorInfo.rcWork,frame),"Actual hidden window was outside its monitor work area");
    const auto expected=multitap::initialWindowFrame(monitorInfo.rcWork,dpi,60,50,host?696:1440,host?800:900);
    require(expected&&frame.left==expected->left&&frame.top==expected->top&&frame.right==expected->right&&frame.bottom==expected->bottom,"Production CreateWindow did not use its fitted frame");
    MINMAXINFO minimum{};SendMessageW(window.GetHandle(),WM_GETMINMAXINFO,0,LPARAM(&minimum));
    const auto expectedMinimum=multitap::minimumWindowTrackSize(monitorInfo.rcWork,GetDpiForWindow(window.GetHandle()),host?600:760,host?520:560);
    require(expectedMinimum&&minimum.ptMinTrackSize.x==expectedMinimum->cx&&minimum.ptMinTrackSize.y==expectedMinimum->cy,"Production resize limits ignored the product role or work area");
    require(!IsWindowVisible(window.GetHandle()),"Geometry QA unexpectedly showed a product window");
    window.Destroy();
  }
  MSG message{};require(!PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE),"Destroying a non-quitting window exited the application");
  return windows;
}
}
int main(){try{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  simulatedFrames();const auto windows=actualWindows();
  std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"simulatedWorkAreas\":7,\"dpiCases\":8,\"roles\":2,\"actualProductionHiddenWindows\":"<<windows<<",\"actualFlutterMonitorDpiQueried\":true,\"productionMinMaxMessageHandled\":true,\"ordinaryFittingSizePreserved\":true,\"displayModesChanged\":false,\"userInputInjected\":false,\"fullFlutterUiVisuallyVerified\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
