#pragma once
#include <windows.h>
#include <cstdint>
#include <climits>
#include <string>
#include <string_view>

namespace display_json {
// JSON crosses PowerShell 5/native process boundaries whose decoding may use
// the current OEM/ANSI code page. ASCII-only escapes preserve both Unicode and
// JSON delimiters regardless of that decoder. No console code page is changed.
inline std::string quote(std::wstring_view text) {
  static constexpr char digits[]="0123456789abcdef";
  std::string out="\"";
  const auto escape=[&](uint16_t unit) {
    out+="\\u";
    for(int shift=12;shift>=0;shift-=4)out+=digits[(unit>>shift)&15];
  };
  for(size_t i=0;i<text.size();++i) {
    const auto unit=uint16_t(text[i]);
    if(unit=='\"'||unit=='\\') {out+='\\';out+=char(unit);}
    else if(unit>=0x20&&unit<=0x7e)out+=char(unit);
    else if(unit>=0xd800&&unit<=0xdbff) {
      if(i+1<text.size()&&uint16_t(text[i+1])>=0xdc00&&uint16_t(text[i+1])<=0xdfff) {
        escape(unit);escape(uint16_t(text[++i]));
      } else escape(0xfffd);
    } else if(unit>=0xdc00&&unit<=0xdfff)escape(0xfffd);
    else escape(unit);
  }
  out+='\"';return out;
}
inline std::string quoteUtf8(std::string_view text) {
  if(text.empty())return "\"\"";
  if(text.size()>INT_MAX)return "\"display_operation_failed\"";
  const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
  // Exception text is diagnostic only. Invalid external byte strings must not
  // produce malformed JSON or be guessed using a different code page.
  if(!length)return "\"display_operation_failed\"";
  std::wstring wide(size_t(length),L'\0');
  if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),length)!=length)return "\"display_operation_failed\"";
  return quote(wide);
}
}
