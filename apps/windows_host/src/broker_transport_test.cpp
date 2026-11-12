// Actual production JSON adapter with memory input only. Never call brokerHealth,
// credentials/settings, WinHTTP, registry, LSA, service, display or user APIs.
#include "support.hpp"
#include <iostream>
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<label<<'\n';}}
bool accepts(const char* profile,bool secure=false){
  try{
    const auto origin=host::transport::parse(secure?"https://example.test":"http://example.test:18400");
    const auto text=std::string(R"({"service":"multitap-broker","status":"ok","authentication":"id-password","media":"relay-v1")")+profile+"}";
    host::validateBrokerHealth(origin,host::JsonParser::parse(text));return true;
  }catch(const std::exception&){return false;}
}
std::string reason(const char* extra,bool secure=false){
  try{
    const auto origin=host::transport::parse(secure?"https://example.test":"http://example.test:18400");
    const auto text=std::string(R"({"service":"multitap-broker","status":"ok","authentication":"id-password","media":"relay-v1")")+extra+"}";
    host::validateBrokerHealth(origin,host::JsonParser::parse(text));return {};
  }catch(const std::exception& error){return error.what();}
}
}
int main(){
  check(accepts("",true),"existing HTTPS health has no new metadata requirement");
  check(accepts(",\"transport\":null",true),"HTTPS existing semantics ignore optional metadata");
  check(!accepts(""),"old backend health cannot authorize HTTP");
  check(accepts(R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":true,"directTls":false})"),"exact HTTP profile accepted");
  for(const auto* profile:{
    R"(,"transport":null)",R"(,"transport":[])",R"(,"transport":true)",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":true})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":true,"directTls":false,"extra":true})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":"true","directTls":false})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":true,"directTls":0})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://other.test:18400","scheme":"http","allowInsecureHttp":true,"directTls":false})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18401","scheme":"http","allowInsecureHttp":true,"directTls":false})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"https://example.test:18400","scheme":"https","allowInsecureHttp":true,"directTls":false})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"HTTP://EXAMPLE.test:18400/","scheme":"http","allowInsecureHttp":true,"directTls":false})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":false,"directTls":false})",
    R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":true,"directTls":true})"})
    check(!accepts(profile),"malformed or inconsistent HTTP profile refused before authorization");
  for(const auto* text:{R"({"service":"other","status":"ok","authentication":"id-password","media":"relay-v1"})",R"({"service":"multitap-broker","status":"error","authentication":"id-password","media":"relay-v1"})",R"({"service":"multitap-broker","status":"ok","authentication":"other","media":"relay-v1"})",R"({"service":"multitap-broker","status":"ok","authentication":"id-password","media":"other"})"}){
    bool rejected=false;try{host::validateBrokerHealth(host::transport::parse("https://example.test"),host::JsonParser::parse(text));}catch(const std::exception&){rejected=true;}
    check(rejected,"existing HTTPS broker compatibility checks remain required");
  }
  check(reason("")=="broker_http_unsupported","only unrecognized old HTTP health is unsupported");
  check(reason(R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://example.test:18400","scheme":"http","allowInsecureHttp":false,"directTls":false})")=="broker_http_disabled","modern disabled HTTP is not old server");
  check(reason(R"(,"transport":{"protocol":"broker-transport-v1","publicOrigin":"http://other.test:18400","scheme":"http","allowInsecureHttp":true,"directTls":false})")=="broker_origin_not_allowed","wrong peer origin is refused distinctly");
  check(reason(R"(,"access":{"protocol":"server-access-v1"})")=="broker_transport_profile_mismatch","recognized access with absent transport is malformed modern response");
  check(reason(R"(,"access":{"protocol":"server-access-v1","bootstrapOnly":true})")=="broker_setup_only","bootstrap HTTP remains setup only");
  check(reason(R"(,"access":{"protocol":"server-access-v1","bootstrapOnly":true})",true).empty(),"HTTPS optional access metadata preserves compatibility");
  check(host::brokerAccessFailure(host::JsonParser::parse(R"({"error":"http_disabled"})"))=="broker_http_disabled","actual HTTP disabled error envelope");
  check(host::brokerAccessFailure(host::JsonParser::parse(R"({"error":"origin_not_allowed"})"))=="broker_origin_not_allowed","actual origin denied error envelope");
  check(host::brokerAccessFailure(host::JsonParser::parse(R"({"error":{"code":"unregistered_origin"}})"))=="broker_origin_not_allowed","legacy structured denial alias");
  check(host::brokerAccessFailure(host::JsonParser::parse(R"({"error":"arbitrary remote text"})")).empty(),"unrecognized error cannot become an authoritative fixed reason");
  check(std::string(host::brokerNetworkFailure(ERROR_WINHTTP_SECURE_FAILURE))=="broker_tls_failed","TLS failure remains TLS failure");
  check(std::string(host::brokerNetworkFailure(ERROR_WINHTTP_CANNOT_CONNECT))=="broker_connection_failed","connect failure remains distinct");
  check(std::string(host::brokerNetworkFailure(ERROR_WINHTTP_TIMEOUT))=="broker_timeout","native timeout remains distinct");
  check(std::string(host::brokerNetworkFailure(ERROR_WINHTTP_NAME_NOT_RESOLVED))=="broker_dns_failed","native DNS failure remains distinct");
  check(std::string(host::brokerNetworkFailure(0xffffffff))=="broker_network_failed","unknown network failure stays generic");
  std::cout<<"{\"kind\":\"host-transport-json-memory\",\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"network\":false,\"settingsRead\":false,\"credentialsRead\":false,\"windowsMutations\":false}\n";
  return failures?1:0;
}
