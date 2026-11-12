#pragma once
#include "support.hpp"
#include <optional>

namespace host {
struct SessionStopRequest {
  std::string session,request;
  bool status=false;
  bool operator==(const SessionStopRequest&)const=default;
};
struct SessionStopEvidence {
  bool workerQuiescent=false,profileRequired=false,profileRestored=false;
};
inline bool sessionStopUuid(const std::string& value){
  if(value.size()!=36)return false;
  for(size_t n=0;n<value.size();++n){
    if(n==8||n==13||n==18||n==23){if(value[n]!='-')return false;}
    else if(!((value[n]>='0'&&value[n]<='9')||(value[n]>='a'&&value[n]<='f')||(value[n]>='A'&&value[n]<='F')))return false;
  }
  return true;
}
inline std::optional<SessionStopRequest> parseSessionStopRequest(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields)return {};
  const auto type=value.get("type").string();const bool status=type=="host.session.stop.status";
  if((!status&&type!="session.stop")||value.get("sessionStopProtocol").string()!="stop-v1"||
     fields->size()!=(status?4:5)||!sessionStopUuid(value.get("sessionId").string())||
     !sessionStopUuid(value.get("stopRequestId").string()))return {};
  for(const auto&[key,_]:*fields)if(key!="type"&&key!="sessionId"&&key!="sessionStopProtocol"&&key!="stopRequestId"&&(!(!status&&key=="reason")))return {};
  if(!status){const auto* reason=std::get_if<std::string>(&value.get("reason").value);
    if(!reason||reason->empty()||reason->size()>80||reason->find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)return {};}
  return SessionStopRequest{value.get("sessionId").string(),value.get("stopRequestId").string(),status};
}
inline std::string sessionStopPacket(const SessionStopRequest& request,SessionStopEvidence evidence){
  // Local uncertainty must not become a restoration acknowledgement.
  evidence.profileRestored=evidence.workerQuiescent&&evidence.profileRestored;
  return "{\"type\":\"host.session.stopped\",\"sessionId\":"+jsonQuote(request.session)+
    ",\"sessionStopProtocol\":\"stop-v1\",\"stopRequestId\":"+jsonQuote(request.request)+
    ",\"workerQuiescent\":"+(evidence.workerQuiescent?"true":"false")+
    ",\"profileRequired\":"+(evidence.profileRequired?"true":"false")+
    ",\"profileRestored\":"+(evidence.profileRestored?"true":"false")+'}';
}
class SessionStopBinding {
 public:
  bool retire(const SessionStopRequest& request,const std::string& current){
    if(request.status||request.session!=current)return false;
    retired_=request;return true;
  }
  bool query(const SessionStopRequest& request)const{
    return request.status&&retired_&&request.session==retired_->session&&request.request==retired_->request;
  }
 private:std::optional<SessionStopRequest> retired_;
};
}
