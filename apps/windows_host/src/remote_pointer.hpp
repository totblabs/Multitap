#pragma once
#include "support.hpp"
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

namespace host {
using RemotePointerBounds = std::function<RECT()>;
struct RemotePointerInput {
  INPUT input{};
  std::string button;
  bool down=false;
};
inline bool remotePointerButton(const std::string& value) {
  return value=="left"||value=="middle"||value=="right"||value=="x1"||value=="x2";
}
// One button change. A side button shares its flags: mouseData says which one.
inline void remotePointerButtonInput(MOUSEINPUT& input,const std::string& button,bool down) {
  if(button=="x1"||button=="x2") {
    input.dwFlags=down?MOUSEEVENTF_XDOWN:MOUSEEVENTF_XUP;
    input.mouseData=button=="x1"?XBUTTON1:XBUTTON2;
    return;
  }
  input.dwFlags=button=="left"?(down?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP):
    button=="right"?(down?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_RIGHTUP):
    (down?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_MIDDLEUP);
}
// relative-mouse-v1 (docs/remote-session-protocol.md): movement of a
// mouse the client captured, not a position. Only a session.start carrying the
// granted marker accepts it; independent outputs keep positions.
inline constexpr const char* relativeMouseProtocol="relative-mouse-v1";
inline constexpr double relativeMouseMaximum=16384;
inline bool relativeMouseSessionRequested(const Json& start) {
  return start.get("type").string()=="session.start"&&start.get("mode").string()=="stream"&&
         start.get("relativeMouseProtocol").string()==relativeMouseProtocol&&start.get("mediaProtocol").string().empty();
}
inline RECT remotePointerDesktopBounds() {
  const LONG left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int64_t right=int64_t(left)+GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int64_t bottom=int64_t(top)+GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if(right<std::numeric_limits<LONG>::min()||right>std::numeric_limits<LONG>::max()||bottom<std::numeric_limits<LONG>::min()||bottom>std::numeric_limits<LONG>::max())return {};
  return {left,top,LONG(right),LONG(bottom)};
}

// Validate the complete packet before querying geometry or emitting input.
// A positioned click/wheel uses one MOUSEINPUT with MOVE and action flags,
// so a rejected movement cannot be followed by a press at an old position.
// https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-mouseinput
inline std::optional<RemotePointerInput> planRemotePointerInput(
    const Json& message,const RECT& source,const RemotePointerBounds& bounds) {
  const auto* fields=std::get_if<Json::Object>(&message.value);
  if(!fields)return {};
  const auto kind=message.get("event").string();
  RemotePointerInput result;result.input.type=INPUT_MOUSE;
  if(kind=="moveRelative") {
    const auto delta=[&](const char* name)->std::optional<LONG> {
      const auto* value=std::get_if<double>(&message.get(name).value);
      if(!value||!std::isfinite(*value)||std::floor(*value)!=*value||std::abs(*value)>relativeMouseMaximum)return {};
      return LONG(*value);
    };
    const auto dx=delta("dx"),dy=delta("dy");
    // Without a bound source there is no owned output to move on.
    if(!dx||!dy||(*dx==0&&*dy==0)||fields->contains("x")||fields->contains("y")||int64_t(source.right)-source.left<=1||int64_t(source.bottom)-source.top<=1)return {};
    result.input.mi.dx=*dx;result.input.mi.dy=*dy;result.input.mi.dwFlags=MOUSEEVENTF_MOVE;
    return result;
  }
  if(kind=="button") {
    result.button=message.get("button").string();
    if(!remotePointerButton(result.button)||!std::holds_alternative<bool>(message.get("down").value))return {};
    result.down=message.get("down").boolean();
    remotePointerButtonInput(result.input.mi,result.button,result.down);
  } else if(kind=="wheel") {
    const auto* delta=std::get_if<double>(&message.get("delta").value);
    if(!delta||!std::isfinite(*delta)||*delta<-12000||*delta>12000||std::floor(*delta)!=*delta)return {};
    result.input.mi.dwFlags=MOUSEEVENTF_WHEEL;
    result.input.mi.mouseData=DWORD(LONG(*delta));
  } else if(kind!="move")return {};

  const bool hasX=fields->contains("x"),hasY=fields->contains("y");
  // Bare legacy button/wheel packets refer to the current cursor. Positional
  // packets must never silently become bare actions after malformed geometry.
  if(!hasX&&!hasY)return kind=="move"?std::nullopt:std::optional{result};
  const auto* x=std::get_if<double>(&message.get("x").value);
  const auto* y=std::get_if<double>(&message.get("y").value);
  if(!hasX||!hasY||!x||!y||!std::isfinite(*x)||!std::isfinite(*y)||*x<0||*x>1||*y<0||*y>1||!bounds)return {};
  RECT desktop{};try{desktop=bounds();}catch(...){return {};}
  const int64_t width=int64_t(desktop.right)-desktop.left,height=int64_t(desktop.bottom)-desktop.top;
  const int64_t sourceWidth=int64_t(source.right)-source.left,sourceHeight=int64_t(source.bottom)-source.top;
  if(width<=1||height<=1||sourceWidth<=1||sourceHeight<=1||source.left<desktop.left||source.top<desktop.top||source.right>desktop.right||source.bottom>desktop.bottom)return {};
  result.input.mi.dx=LONG((source.left+*x*double(sourceWidth-1)-desktop.left)*65535/double(width-1));
  result.input.mi.dy=LONG((source.top+*y*double(sourceHeight-1)-desktop.top)*65535/double(height-1));
  result.input.mi.dwFlags|=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
  return result;
}
}
