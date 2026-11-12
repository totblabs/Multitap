#pragma once
#include "console_browser_target.h"
#include <string_view>

namespace multitap {
struct ConsoleWebViewTarget {
  std::string origin, desktop_id, page, attach, code;
};
inline std::optional<ConsoleWebViewTarget> ConsoleWebViewDestination(
    const std::string& origin,const std::string& id,const std::string& path,const std::string& code) {
  const auto page=ConsoleBrowserTarget(origin,id);
  if(!page||path!="/console/"+id+"/attach"||code.size()!=43)return {};
  for(const char c:code)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return {};
  const auto base=page->substr(0,page->size()-std::string("/console/").size()-id.size());
  return ConsoleWebViewTarget{base,id,*page,base+path,code};
}
inline bool ConsoleWebViewNavigation(const ConsoleWebViewTarget& target,std::string_view url,bool bootstrap) {
  return url==target.page||(bootstrap&&url==target.attach);
}
inline bool ConsoleWebViewAttachPost(const ConsoleWebViewTarget& target,std::string_view url,std::string_view method,bool initial,bool document){
  const auto expected=ConsoleBrowserTarget(target.origin,target.desktop_id);
  return initial&&document&&method=="POST"&&expected&&target.page==*expected&&
    target.origin+"/console/"+target.desktop_id==*expected&&target.attach==*expected+"/attach"&&url==target.attach;
}
inline bool ConsoleWebViewResource(const ConsoleWebViewTarget& target,std::string_view url) {
  const auto prefix=target.origin+"/console/";
  if(url.substr(0,prefix.size())!=prefix||url.find_first_of("?#\\")!=std::string_view::npos)return false;
  const auto path=url.substr(target.origin.size());
  // No encoded path separators or parent traversal can widen this origin/path.
  return path.find('%')==std::string_view::npos&&path.find("/../")==std::string_view::npos&&path.find("/./")==std::string_view::npos;
}
}
