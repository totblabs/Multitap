#pragma once
#include "stream_resize.hpp"
namespace host {
// A single stream selects output 0; it does not remove the other owned output.
inline bool bindDisplayV2SingleCapture(Json& command,const DisplayState& display,DWORD width,DWORD height,const std::vector<CaptureMonitor>& observed){
  if(!display.ready()||display.remoteSession||display.monitors.empty()||display.monitors.size()>2)return false;
  const auto monitor=std::find_if(display.monitors.begin(),display.monitors.end(),[](const auto& item){return item.id=="0";});
  if(monitor==display.monitors.end()||std::count_if(display.monitors.begin(),display.monitors.end(),[](const auto& item){return item.id=="0";})!=1||
     monitor->width!=width||monitor->height!=height||!monitor->primary||
     (!command.get("monitor").string().empty()&&command.get("monitor").string()!="0"))return false;
  try{
    auto bound=command;streamField(bound,"monitor",std::string("0"));
    const auto selected=attachFreshCapture(bound,display,observed);
    if(selected.monitors.size()!=1||selected.monitors.front().name!=monitor->name||
       int64_t(selected.bounds.right)-selected.bounds.left!=width||int64_t(selected.bounds.bottom)-selected.bounds.top!=height)return false;
    command=std::move(bound);return true;
  }catch(...){return false;}
}
}
