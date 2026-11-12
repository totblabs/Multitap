#include "media.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>

static double millis(host::Clock::time_point a,host::Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
static double percentile(std::vector<double> v,double p){std::sort(v.begin(),v.end());return v[std::min(v.size()-1,size_t(std::ceil(p*v.size())-1))];}
static void timer(bool precise){
  HANDLE timer=precise?CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS):nullptr;
  if(precise&&!timer)throw std::runtime_error("high_resolution_timer_unavailable");
  auto begin=host::Clock::now(),next=begin;constexpr int frames=120;
  for(int n=0;n<frames;++n){next+=std::chrono::microseconds(16667);if(timer){const auto delta=std::chrono::duration_cast<std::chrono::nanoseconds>(next-host::Clock::now()).count();if(delta>0){LARGE_INTEGER due;due.QuadPart=-std::max<LONGLONG>(1,delta/100);if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))throw std::runtime_error("timer_set_failed");WaitForSingleObject(timer,1000);}}else std::this_thread::sleep_until(next);}
  const auto ms=millis(begin,host::Clock::now());if(timer)CloseHandle(timer);std::cout<<"{\"test\":\"timer\",\"implementation\":\""<<(precise?"high_resolution_waitable":"std_sleep_until")<<"\",\"iterations\":"<<frames<<",\"elapsedMs\":"<<ms<<",\"achievedFps\":"<<frames*1000/ms<<"}\n";
}
static void pipeline(int width,int height,int bitrate,const std::filesystem::path& destination){
  host::Encoder encoder(width,height,60,bitrate);constexpr int frames=90;
  HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
  uint8_t* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);if(!dc||!bitmap||!pixels)throw std::runtime_error("synthetic_bitmap_failed");const auto previous=SelectObject(dc,bitmap);
  HFONT font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");const auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
  std::vector<uint8_t> raw(size_t(width)*height*3/2);std::vector<double> convert,encode;uint64_t bytes=0,emitted=0;
  std::ofstream stream(destination,std::ios::binary);const auto begin=host::Clock::now();
  const auto output=[&](host::Frame f){const auto size=uint32_t(f.bytes.size());stream.write(reinterpret_cast<const char*>(&size),4);stream.write(reinterpret_cast<const char*>(f.bytes.data()),std::streamsize(size));bytes+=size;++emitted;};
  for(int n=0;n<frames;++n){
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto* p=pixels+(size_t(y)*width+x)*4;const bool stripe=((x+n*7)/90+y/60)%2;p[0]=uint8_t(stripe?240:217);p[1]=uint8_t(stripe?242:225);p[2]=uint8_t(stripe?245:238);p[3]=255;}
    SetTextColor(dc,RGB(18,26,40));for(int y=15;y<height;y+=31){const wchar_t text[]=L"Multitap  한글 문서  ABC abc  0123456789   Virtual desktop quality / 1:1 pixels";TextOutW(dc,17,y,text,int(std::size(text)-1));}
    GdiFlush();const auto a=host::Clock::now();host::bgraToNv12(pixels,raw.data(),width,height);const auto b=host::Clock::now();encoder.encode(raw.data(),uint64_t(n)*1000000/60,output);const auto c=host::Clock::now();convert.push_back(millis(a,b));encode.push_back(millis(b,c));
  }
  encoder.finish(output);const double ms=millis(begin,host::Clock::now());SelectObject(dc,oldFont);SelectObject(dc,previous);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);
  if(emitted!=frames||!bytes||!stream)throw std::runtime_error("incomplete_encoded_stream");
  std::cout<<"{\"test\":\"synthetic_desktop_pipeline\",\"width\":"<<width<<",\"height\":"<<height<<",\"bitrateTarget\":"<<bitrate<<",\"frames\":"<<emitted<<",\"elapsedMs\":"<<ms<<",\"unpacedFps\":"<<frames*1000/ms<<",\"convertMeanMs\":"<<std::accumulate(convert.begin(),convert.end(),0.0)/frames<<",\"convertP95Ms\":"<<percentile(convert,.95)<<",\"encodeMeanMs\":"<<std::accumulate(encode.begin(),encode.end(),0.0)/frames<<",\"encodeP95Ms\":"<<percentile(encode,.95)<<",\"bytes\":"<<bytes<<"}\n";
}
static void captureBench(){
  if(!host::interactiveDesktopAvailable()){std::cout<<"{\"test\":\"capture\",\"skipped\":\"interactive_desktop_unavailable\"}\n";return;}
  const int w=GetSystemMetrics(SM_CXSCREEN)&~1,h=GetSystemMetrics(SM_CYSCREEN)&~1;
  for(const bool scaled:{true,false}){const int width=scaled?1280:w,height=scaled?720:h;host::Capture capture(width,height);std::vector<double> times;int unique=0;const auto begin=host::Clock::now();for(int n=0;n<60;++n){const auto a=host::Clock::now();capture.frame();if(capture.frameUpdated())++unique;times.push_back(millis(a,host::Clock::now()));}const auto ms=millis(begin,host::Clock::now());std::cout<<"{\"test\":\"actual_capture_no_frame_files\",\"backend\":\""<<capture.backend()<<"\",\"backendError\":\""<<capture.backendError()<<"\",\"width\":"<<width<<",\"height\":"<<height<<",\"sourceWidth\":"<<w<<",\"sourceHeight\":"<<h<<",\"captureConvertMeanMs\":"<<ms/60<<",\"captureConvertP95Ms\":"<<percentile(times,.95)<<",\"unpacedFps\":"<<60000/ms<<",\"newCaptureFrames\":"<<unique<<"}\n";}
  HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);const auto previous=SelectObject(memory,bitmap);
  for(const DWORD flags:{DWORD(SRCCOPY|CAPTUREBLT),DWORD(SRCCOPY)}){auto begin=host::Clock::now();for(int n=0;n<60;++n){if(!BitBlt(memory,0,0,w,h,screen,0,0,flags))throw std::runtime_error("blit_benchmark_failed");GdiFlush();}auto ms=millis(begin,host::Clock::now());std::cout<<"{\"test\":\"native_bitblt_no_frame_files\",\"captureBlt\":"<<((flags&CAPTUREBLT)?"true":"false")<<",\"meanMs\":"<<ms/60<<",\"unpacedFps\":"<<60000/ms<<"}\n";}
  SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);
}
static LRESULT CALLBACK qaWindow(HWND window,UINT message,WPARAM w,LPARAM l){
  if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);RECT r{};GetClientRect(window,&r);const auto brush=CreateSolidBrush(RGB(25,50,85));FillRect(dc,&r,brush);DeleteObject(brush);const int n=int(GetWindowLongPtrW(window,GWLP_USERDATA));RECT bar{(n*9)%440,45,(n*9)%440+35,110};FillRect(dc,&bar,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);const wchar_t text[]=L"Multitap capture QA - own animation only";TextOutW(dc,12,12,text,int(std::size(text)-1));EndPaint(window,&ps);return 0;}return DefWindowProcW(window,message,w,l);
}
static void movingCapture(){
  WNDCLASSW c{};c.lpfnWndProc=qaWindow;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"MultitapCapturePerformanceQA";RegisterClassW(&c);
  HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,c.lpszClassName,L"Multitap capture QA - own animation only",WS_POPUP,40,40,500,150,nullptr,nullptr,c.hInstance,nullptr);
  if(!window)throw std::runtime_error("capture_qa_window_unavailable");SetWindowPos(window,HWND_TOPMOST,40,40,500,150,SWP_NOACTIVATE|SWP_SHOWWINDOW);
  try{host::Capture capture(1280,720);int unique=0;constexpr int frames=180;const auto begin=host::Clock::now();auto next=begin;std::vector<double> work;
    for(int n=0;n<frames;++n){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}SetWindowLongPtrW(window,GWLP_USERDATA,n);InvalidateRect(window,nullptr,FALSE);UpdateWindow(window);const auto a=host::Clock::now();capture.frame();work.push_back(millis(a,host::Clock::now()));if(capture.frameUpdated())++unique;next+=std::chrono::microseconds(16667);std::this_thread::sleep_until(next);}
    const auto elapsed=millis(begin,host::Clock::now());std::cout<<"{\"test\":\"actual_own_animation_capture\",\"backend\":\""<<capture.backend()<<"\",\"requests\":"<<frames<<",\"newCaptureFrames\":"<<unique<<",\"elapsedMs\":"<<elapsed<<",\"newFrameFps\":"<<unique*1000/elapsed<<",\"captureConvertMeanMs\":"<<std::accumulate(work.begin(),work.end(),0.0)/frames<<",\"captureConvertP95Ms\":"<<percentile(work,.95)<<",\"frameFilesSaved\":false,\"remoteInputSent\":false}\n";
  }catch(...){DestroyWindow(window);throw;}DestroyWindow(window);UnregisterClassW(c.lpszClassName,c.hInstance);
}
int wmain(int argc,wchar_t**argv){try{SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);if(argc!=2)throw std::runtime_error("output_directory_required");const std::filesystem::path out=argv[1];std::filesystem::create_directories(out);host::Runtime runtime;timer(false);timer(true);pipeline(1280,720,6000000,out/L"720p.h264.frames");pipeline(1584,884,12000000,out/L"native.h264.frames");pipeline(1920,1080,12000000,out/L"1080p.h264.frames");try{pipeline(3840,1080,20000000,out/L"dual.h264.frames");}catch(const std::exception&e){std::cout<<"{\"test\":\"dual_pipeline\",\"error\":\""<<e.what()<<"\"}\n";}captureBench();movingCapture();return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
