#pragma once
#include "signin_bridge_policy.hpp"
#include "json_object.hpp"

namespace host::signin::broker::wire {
template<size_t N>inline std::string hex(const std::array<uint8_t,N>& bytes){
  constexpr char digits[]="0123456789abcdef";std::string out;
  for(const auto value:bytes){out+=digits[value>>4];out+=digits[value&15];}return out;
}
inline const char* phase(Role role){if(role==Role::signin)return "signin";if(role==Role::user)return "desktop";throw std::runtime_error("invalid_signin_phase");}
inline std::string phaseFields(const std::string& session,const Admission& a){
  return "\"protocol\":\"signin-stream-v1\",\"sessionId\":"+jsonQuote(session)+",\"transitionId\":"+jsonQuote(hex(a.transition))+
    ",\"workerGeneration\":"+std::to_string(a.workerGeneration)+",\"mediaScope\":{\"kind\":\"composite\",\"epoch\":"+std::to_string(a.mediaEpoch)+'}';
}
inline std::string binding(const Admission& a,Role role){
  return "{\"protocol\":\"signin-stream-v1\",\"phase\":"+jsonQuote(phase(role))+",\"workerGeneration\":"+std::to_string(a.workerGeneration)+
    ",\"transitionId\":"+jsonQuote(hex(a.transition))+",\"mediaScope\":{\"kind\":\"composite\",\"epoch\":"+std::to_string(a.mediaEpoch)+"}}";
}
inline std::string eligibility(const std::string& instance,uint32_t revision,WindowsState state){
  if(state!=WindowsState::locked&&state!=WindowsState::noUser)throw std::runtime_error("invalid_signin_eligibility");
  // Native trust facts stay in the service. Public JSON is this exact schema.
  return "{\"type\":\"host.signin.eligibility\",\"protocol\":\"signin-service-v1\",\"serviceInstance\":"+jsonQuote(instance)+",\"revision\":"+std::to_string(revision)+
    ",\"windowsState\":"+jsonQuote(state==WindowsState::locked?"locked":"no-user")+",\"outputs\":1,\"validForMs\":5000}";
}
inline std::string eligibilityWithdraw(const std::string& instance,uint32_t revision){
  return "{\"type\":\"host.signin.eligibility.withdraw\",\"protocol\":\"signin-service-v1\",\"serviceInstance\":"+jsonQuote(instance)+",\"revision\":"+std::to_string(revision)+'}';
}
inline std::string prepare(const std::string& session,const Admission& a,Role role,uint32_t timeout=15000){
  return "{\"type\":\"stream.phase.prepare\","+phaseFields(session,a)+",\"phase\":"+jsonQuote(phase(role))+",\"timeoutMs\":"+std::to_string(timeout)+'}';
}
inline std::string info(const std::string& session,const Admission& a,Role role,const PhaseId& frame,const Json& value){
  return "{\"type\":\"stream.phase.info\","+phaseFields(session,a)+",\"phase\":"+jsonQuote(phase(role))+",\"firstFrameId\":"+jsonQuote(hex(frame))+",\"info\":"+jsonObject(value)+'}';
}
inline std::string authority(const std::string& session,const std::string& instance,uint32_t revision,const Admission& a,Role role,bool withdrawn){
  return "{\"type\":\"host.signin.authority\",\"protocol\":\"signin-service-v1\",\"sessionId\":"+jsonQuote(session)+",\"serviceInstance\":"+jsonQuote(instance)+
    ",\"revision\":"+std::to_string(revision)+",\"binding\":"+binding(a,role)+",\"state\":"+jsonQuote(withdrawn?"withdrawn":"current")+",\"validForMs\":1000,\"renewAfterMs\":500}";
}
inline std::string previousExited(const std::string& session,const std::string& instance,const Admission& a,Role role){
  return "{\"type\":\"host.signin.previous-worker-exited\",\"protocol\":\"signin-service-v1\",\"sessionId\":"+jsonQuote(session)+",\"serviceInstance\":"+jsonQuote(instance)+",\"binding\":"+binding(a,role)+'}';
}
}
