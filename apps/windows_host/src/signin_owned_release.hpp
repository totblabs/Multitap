#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
namespace host::signin::entry {
// Used ONLY while RemoteInput::reset walks its own successfully injected
// key/button/text-unit bookkeeping. No public packet supplies this list.
inline bool ownedReleaseShape(const INPUT& value){
  if(value.type==INPUT_KEYBOARD){
    constexpr DWORD allowed=KEYEVENTF_KEYUP|KEYEVENTF_SCANCODE|KEYEVENTF_EXTENDEDKEY|KEYEVENTF_UNICODE;
    return (value.ki.dwFlags&KEYEVENTF_KEYUP)&&!(value.ki.dwFlags&~allowed)&&
      !((value.ki.dwFlags&KEYEVENTF_UNICODE)&&(value.ki.dwFlags&KEYEVENTF_SCANCODE));
  }
  if(value.type==INPUT_MOUSE){
    constexpr DWORD allowed=MOUSEEVENTF_LEFTUP|MOUSEEVENTF_RIGHTUP|MOUSEEVENTF_MIDDLEUP|MOUSEEVENTF_XUP;
    return value.mi.dwFlags&&!(value.mi.dwFlags&~allowed)&&value.mi.dx==0&&value.mi.dy==0&&
      ((value.mi.dwFlags&MOUSEEVENTF_XUP)?(value.mi.mouseData==XBUTTON1||value.mi.mouseData==XBUTTON2):value.mi.mouseData==0);
  }
  return false;
}
inline constexpr uint32_t releasePendingExitCode=0x201;
}
