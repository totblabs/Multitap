#pragma once
#include "machine.hpp"
namespace host {
struct CaptureMonitor { std::wstring name, identity; RECT bounds{}; bool primary=false; };
struct CaptureSelection { RECT bounds{}; std::vector<CaptureMonitor> monitors; };
inline bool sameRect(RECT a,RECT b){return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;}
inline bool intersects(RECT a,RECT b){return std::max(a.left,b.left)<std::min(a.right,b.right)&&std::max(a.top,b.top)<std::min(a.bottom,b.bottom);}
inline std::vector<CaptureMonitor> currentCaptureMonitors(){
  struct State{std::vector<CaptureMonitor> monitors;bool failed=false;} state;
  const BOOL ok=EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM value)->BOOL{
    auto& state=*reinterpret_cast<State*>(value);MONITORINFOEXW info{};info.cbSize=sizeof(info);
    if(!GetMonitorInfoW(monitor,reinterpret_cast<MONITORINFO*>(&info))){state.failed=true;return FALSE;}
    std::wstring identity;
    for(DWORD i=0;i<64;i++){DISPLAY_DEVICEW adapter{};adapter.cb=sizeof(adapter);if(!EnumDisplayDevicesW(nullptr,i,&adapter,0))break;if(_wcsicmp(adapter.DeviceName,info.szDevice)==0){identity=std::wstring(adapter.DeviceID)+L"|"+adapter.DeviceKey;break;}}
    if(identity.empty()||identity==L"|"){state.failed=true;return FALSE;}
    try{state.monitors.push_back({info.szDevice,std::move(identity),info.rcMonitor,(info.dwFlags&MONITORINFOF_PRIMARY)!=0});}catch(...){state.failed=true;return FALSE;}return TRUE;
  },reinterpret_cast<LPARAM>(&state));
  if(!ok||state.failed||state.monitors.empty()||state.monitors.size()>64)throw std::runtime_error("Cannot validate the selected virtual monitor topology");return state.monitors;
}
inline void validateCaptureSelection(const CaptureSelection& selected,const std::vector<CaptureMonitor>& observed){
  if(selected.monitors.empty()||selected.monitors.size()>2)throw std::runtime_error("Only the selected owned virtual monitors can be captured");
  uint64_t area=0;
  for(size_t i=0;i<selected.monitors.size();i++){
    const auto& item=selected.monitors[i];const auto rect=item.bounds;
    const auto width=int64_t(rect.right)-rect.left,height=int64_t(rect.bottom)-rect.top;
    if(item.name.rfind(L"\\\\.\\DISPLAY",0)!=0||item.identity.empty()||!checkedFrameLayout(uint64_t(width),uint64_t(height),false))throw std::runtime_error("Invalid owned virtual monitor geometry");
    if(rect.left<selected.bounds.left||rect.top<selected.bounds.top||rect.right>selected.bounds.right||rect.bottom>selected.bounds.bottom)throw std::runtime_error("Selected monitor lies outside the validated capture area");
    for(size_t j=0;j<i;j++)if(item.name==selected.monitors[j].name||intersects(rect,selected.monitors[j].bounds))throw std::runtime_error("Overlapping virtual monitors cannot be captured");
    size_t matched=0;for(const auto& actual:observed){if(actual.name==item.name&&actual.identity==item.identity&&sameRect(actual.bounds,rect))matched++;}if(matched!=1)throw std::runtime_error("Selected virtual monitor changed; reconnect the session");
    area+=uint64_t(width)*uint64_t(height);
  }
  const auto bounds=selected.bounds;
  const auto width=int64_t(bounds.right)-bounds.left,height=int64_t(bounds.bottom)-bounds.top;
  if(!checkedFrameLayout(uint64_t(width),uint64_t(height),false)||area!=uint64_t(width)*uint64_t(height))throw std::runtime_error("Virtual monitors must be adjacent; gaps or console displays are not captured");
  for(const auto& actual:observed){if(!intersects(bounds,actual.bounds))continue;bool selectedMonitor=false;for(const auto& item:selected.monitors)if(item.name==actual.name&&item.identity==actual.identity&&sameRect(item.bounds,actual.bounds))selectedMonitor=true;if(!selectedMonitor)throw std::runtime_error("A console or unselected monitor intersects the capture area");}
}
inline CaptureSelection selectOwnedCapture(const DisplayState& display,const std::string& requested,const std::vector<CaptureMonitor>& observed){
  if(!display.ready()||display.monitors.size()>2)throw std::runtime_error("Owned virtual displays are required");
  const auto id=requested.empty()?"0":requested;if(id!="0"&&id!="1"&&id!="all")throw std::runtime_error("Unsupported virtual monitor selection");
  CaptureSelection selection;bool first=true;
  for(const auto& monitor:display.monitors){if(monitor.id!="0"&&monitor.id!="1")throw std::runtime_error("Unknown owned virtual monitor");if(id!="all"&&monitor.id!=id)continue;
    if(!displayRectangleFits(monitor.left,monitor.top,monitor.width,monitor.height))throw std::runtime_error("Invalid owned virtual monitor geometry");
    const RECT bounds{monitor.left,monitor.top,LONG(int64_t(monitor.left)+monitor.width),LONG(int64_t(monitor.top)+monitor.height)};std::wstring identity;for(const auto& actual:observed)if(actual.name==monitor.name&&sameRect(actual.bounds,bounds))identity=actual.identity;
    selection.monitors.push_back({monitor.name,std::move(identity),bounds});if(first){selection.bounds=bounds;first=false;}else{selection.bounds.left=std::min(selection.bounds.left,bounds.left);selection.bounds.top=std::min(selection.bounds.top,bounds.top);selection.bounds.right=std::max(selection.bounds.right,bounds.right);selection.bounds.bottom=std::max(selection.bounds.bottom,bounds.bottom);}
  }
  if(id=="all"&&selection.monitors.size()!=2)throw std::runtime_error("Both owned virtual monitors are required for all selection");validateCaptureSelection(selection,observed);return selection;
}
inline CaptureSelection parseCaptureSelection(const Json& request,RECT source){
  CaptureSelection selection;selection.bounds=source;const auto* values=std::get_if<Json::Array>(&request.get("captureDisplays").value);if(!values||values->size()>2)throw std::runtime_error("Validated owned monitor selection is required");
  for(const auto& item:*values){const auto coordinate=[&](const char* key){const double value=item.get(key).number(double(INT32_MIN)-1);if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX||std::floor(value)!=value)throw std::runtime_error("Invalid monitor coordinate");return LONG(value);};selection.monitors.push_back({wide(item.get("name").string()),wide(item.get("identity").string()),{coordinate("left"),coordinate("top"),coordinate("right"),coordinate("bottom")}});}return selection;
}
}
