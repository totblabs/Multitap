#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>

namespace multitap {
struct LocalMonitorSurface { RECT bounds{}; bool primary=false; UINT dpi=96; };
inline std::vector<LocalMonitorSurface> LocalMonitorLayout() {
  std::vector<LocalMonitorSurface> monitors;
  if(!EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR handle,HDC,LPRECT,LPARAM context)->BOOL {
    MONITORINFO info{sizeof(info)};
    if(!GetMonitorInfoW(handle,&info))return FALSE;
    UINT dpi=96, vertical=96;
    // Optional DPI observation; geometry remains physical monitor pixels.
    using QueryDpi=HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*);
    if(const auto shcore=LoadLibraryExW(L"shcore.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)){
      if(const auto query=reinterpret_cast<QueryDpi>(GetProcAddress(shcore,"GetDpiForMonitor")))
        if(FAILED(query(handle,0,&dpi,&vertical)))dpi=96;
      FreeLibrary(shcore);
    }
    auto& values=*reinterpret_cast<std::vector<LocalMonitorSurface>*>(context);
    values.push_back({info.rcMonitor,(info.dwFlags&MONITORINFOF_PRIMARY)!=0,dpi});
    return values.size()<=16;
  },reinterpret_cast<LPARAM>(&monitors)))return {};
  std::sort(monitors.begin(),monitors.end(),[](const auto& a,const auto& b){
    if(a.primary!=b.primary)return a.primary;
    if(a.bounds.left!=b.bounds.left)return a.bounds.left<b.bounds.left;
    return a.bounds.top<b.bounds.top;
  });
  return monitors;
}
}
