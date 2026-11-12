#pragma once
#include "stream_resize.hpp"
#include "native_display_activation.hpp"
namespace host {
inline bool nativeSameResize(const StreamResize& a,const StreamResize& b){
  return a.sessionId==b.sessionId&&a.requestId==b.requestId&&a.resizeId==b.resizeId&&a.monitor==b.monitor&&a.width==b.width&&a.height==b.height&&a.fps==b.fps&&a.epoch==b.epoch;
}
inline std::string nativeResizeFields(const StreamResize& request){
  return streamResizeFields(request)+",\"monitor\":"+jsonQuote(request.monitor)+",\"width\":"+std::to_string(request.width)+",\"height\":"+std::to_string(request.height)+",\"fps\":"+std::to_string(request.fps);
}
inline bool nativeResizeValid(const StreamResize& request){
  try{return nativeSameResize(request,streamResizeRequest(JsonParser::parse('{'+nativeResizeFields(request)+'}')));}catch(...){return false;}
}
struct NativeViewportReply {
  bool accepted=false,driverChangeAttempted=false,pending=false,recoveryRequired=false;
  std::string snapshot;
};
}
