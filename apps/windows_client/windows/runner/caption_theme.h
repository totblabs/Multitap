#pragma once
#include <windows.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace multitap {
// Every Multitap window's caption, without changing the user's Windows theme:
// charcoal when the app is dark, near white when it is light. A window that
// sets only the dark-mode flag gets the Windows accent colour instead.
inline void ApplyCaptionTheme(HWND window,bool dark){
  const BOOL dark_caption=dark?TRUE:FALSE;
  const COLORREF caption_color=dark?RGB(14,20,24):RGB(243,247,250);
  const COLORREF text_color=dark?RGB(233,241,243):RGB(25,38,45);
  DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark_caption,sizeof(dark_caption));
  DwmSetWindowAttribute(window,DWMWA_CAPTION_COLOR,&caption_color,sizeof(caption_color));
  DwmSetWindowAttribute(window,DWMWA_TEXT_COLOR,&text_color,sizeof(text_color));
}
}
