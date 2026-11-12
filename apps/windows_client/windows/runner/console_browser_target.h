#pragma once
#include <optional>
#include <string>

namespace multitap {
// A deliberately narrow HTTPS origin + ASCII desktop ID. Never accepts a
// caller-supplied path, userinfo, query, fragment, command or shell arguments.
inline std::optional<std::string> ConsoleBrowserTarget(const std::string& origin,
                                                      const std::string& id) {
  const auto alnum=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
  if(id.empty()||id.size()>80||!alnum(id[0]))return std::nullopt;
  for(char c:id)if(!alnum(c)&&c!='_'&&c!='-')return std::nullopt;
  if(origin.size()<9||origin.size()>2048||origin.compare(0,8,"https://")!=0)return std::nullopt;
  std::string authority=origin.substr(8);
  if(!authority.empty()&&authority.back()=='/')authority.pop_back();
  if(authority.empty())return std::nullopt;
  std::string host,port;
  if(authority[0]=='['){
    const auto end=authority.find(']');
    if(end==std::string::npos||end<2)return std::nullopt;
    host=authority.substr(1,end-1);
    if(host.find(':')==std::string::npos)return std::nullopt;
    for(char c:host)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')||c==':'||c=='.'))return std::nullopt;
    if(end+1<authority.size()){if(authority[end+1]!=':')return std::nullopt;port=authority.substr(end+2);if(port.empty())return std::nullopt;}
  }else{
    const auto colon=authority.find(':');
    host=authority.substr(0,colon);
    for(char c:host)if(!alnum(c)&&c!='.'&&c!='-')return std::nullopt;
    if(colon!=std::string::npos){port=authority.substr(colon+1);if(port.empty())return std::nullopt;}
  }
  if(host.empty())return std::nullopt;
  if(!port.empty()){
    if(port.size()>5)return std::nullopt;
    unsigned number=0;for(char c:port){if(c<'0'||c>'9')return std::nullopt;number=number*10+unsigned(c-'0');}
    if(number==0||number>65535)return std::nullopt;
  }
  return "https://"+authority+"/console/"+id;
}
}
