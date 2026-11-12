#include "../../apps/windows_host/src/broker_origin.hpp"
#include <iostream>

namespace {
using namespace host::transport;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<label<<'\n';}}
template<class Action> void rejects(Action action,const char* label){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,label);}
void run(){
  struct Vector{const char* input;const char* normalized;uint16_t port;bool secure;};
  for(const auto& vector:std::vector<Vector>{
    {"HTTPS://EXAMPLE.test:443/","https://example.test",443,true},
    {"https://example.test:8443","https://example.test:8443",8443,true},
    {"http://192.0.2.10:18400/","http://192.0.2.10:18400",18400,false},
    {"http://192.0.2.10","http://192.0.2.10",80,false},
    {"http://192.0.2.10:80/","http://192.0.2.10",80,false},
    {"http://[2001:0DB8:0:0:0:0:0:1]:18400/","http://[2001:db8::1]:18400",18400,false},
    {"https://[0:0:0:0:0:0:0:1]:443","https://[::1]",443,true},
    {"http://[::ffff:192.0.2.1]","http://[::ffff:c000:201]",80,false},
    {"https://example.test./","https://example.test.",443,true},
    {"https://xn--bcher-kva.example","https://xn--bcher-kva.example",443,true}}){
    const auto value=parse(vector.input);check(value.text==vector.normalized&&value.port==vector.port&&value.secure()==vector.secure,"canonical scheme host effective port");
    check(parse(value.text).text==value.text,"canonical origin idempotent");
  }
  for(const auto& input:std::vector<std::string>{
    "","https://","https://example.test:","http://example.test:0","http://example.test:65536",
    "http://example.test:+80","http://example.test:-80","http://example.test:1e3","http://example.test:0x50",
    "https://user@example.test","https://user:pass@example.test","https://example.test/path","https://example.test//",
    "https://example.test?","https://example.test#","https://example.test\\evil"," https://example.test","https://example.test ",
    "https://exam\tple.test","https://example.test\r\n","https://example%2etest","ftp://example.test",
    "http://127.1","http://0177.0.0.1","http://0x7f.0.0.1","http://2130706433","http://256.0.0.1",
    "http://[2001:db8::1%2]","http://2001:db8::1","http://[::1]:","http://[:::1]","http://[::1::2]",
    "http://[1:2:3]","http://[1:2:3:4:5:6:7:8:9]","http://[::ffff:192.000.2.1]",
    std::string("http://example.test\0.evil",25)})rejects([&]{(void)parse(input);},"ambiguous or non-origin input rejected");
  const auto plain=authorize("http://EXAMPLE.test:80/","http://example.test");
  check(plain.text=="http://example.test"&&!plain.secure(),"explicit equivalent HTTP consent");
  for(const auto& consent:std::vector<std::string>{"","https://example.test","http://other.test","http://example.test:81"})
    rejects([&]{(void)authorize("http://example.test",consent);},"HTTP consent is exact scheme host effective port");
  check(authorize("https://example.test").secure(),"HTTPS has no new consent requirement");
  const auto tls=parse("https://example.test:18400");const auto http=authorize("http://example.test:18400","http://example.test:18400");
  check(!relayMatches(http,"ws://example.test:18400/api/../secret","/api/../secret"),"expected path cannot carry dot segments");
  check(!relayMatches(http,"ws://example.test:18400/api/%2fsecret","/api/%2fsecret"),"expected path cannot hide encoded delimiters");
  const std::string base="/api/v1/hosts/sessions/11111111-2222-4333-8444-555555555555";
  for(const auto& suffix:std::vector<std::string>{"","/clipboard","/drive","/tracks","/print","/usb"}){
    const auto path=base+suffix;
    check(relayMatches(tls,"wss://EXAMPLE.test:18400"+path,path),"HTTPS exact WSS path");
    check(relayMatches(http,"ws://example.test:18400"+path,path),"HTTP exact WS path");
    check(!relayMatches(tls,"ws://example.test:18400"+path,path),"no HTTPS to WS downgrade");
    check(!relayMatches(http,"wss://example.test:18400"+path,path),"no automatic scheme conversion");
    for(const auto& prefix:std::vector<std::string>{"ws://other.test:18400","ws://example.test:18401","ws://user@example.test:18400"})
      check(!relayMatches(http,prefix+path,path),"relay origin cannot escape consent");
    check(!relayMatches(http,"ws://example.test:18400"+path+"?token=x",path),"relay query rejected");
    check(!relayMatches(http,"ws://example.test:18400"+path+"/extra",path),"relay path exact");
    check(relaySyntax("ws://example.test:18400"+path,path),"syntax-only grant extraction permits explicit WS");
    check(!relaySyntax("http://example.test:18400"+path,path),"HTTP URL is not a WS grant");
  }
  HealthProfile profile;profile.exactTypes=true;profile.protocol="broker-transport-v1";profile.publicOrigin=http.text;profile.scheme="http";profile.allowInsecureHttp=true;
  check(healthMatches(http,profile,true),"exact configured HTTP transport profile");
  check(!healthMatches(http,profile,false),"old backend health cannot authorize HTTP");
  auto bad=profile;bad.exactTypes=false;check(!healthMatches(http,bad,true),"malformed fields rejected");
  bad=profile;bad.publicOrigin="http://example.test:18401";check(!healthMatches(http,bad,true),"health port mismatch rejected");
  bad=profile;bad.publicOrigin="HTTP://EXAMPLE.test:18400/";check(!healthMatches(http,bad,true),"advertised origin must already be canonical");
  bad=profile;bad.scheme="https";check(!healthMatches(http,bad,true),"health scheme mismatch rejected");
  bad=profile;bad.allowInsecureHttp=false;check(!healthMatches(http,bad,true),"server opt-in required");
  bad=profile;bad.directTls=true;check(!healthMatches(http,bad,true),"HTTP cannot claim direct TLS");
  bad=profile;bad.protocol="broker-transport-v2";check(!healthMatches(http,bad,true),"unknown transport protocol rejected");
  check(healthMatches(tls,{},false),"legacy HTTPS health remains compatible");
  check(healthMatches(tls,bad,true),"optional new metadata cannot reject established HTTPS health");
  profile.publicOrigin=tls.text;profile.scheme="https";profile.allowInsecureHttp=false;
  check(healthMatches(tls,profile,true),"HTTPS proxy transport profile");profile.directTls=true;
  check(healthMatches(tls,profile,true),"HTTPS direct TLS transport profile");
}
}
int main(){try{run();}catch(const std::exception&){check(false,"unexpected fixture exception");}
  std::cout<<"{\"kind\":\"host-broker-transport-policy\",\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"nativeCalls\":false,\"network\":false,\"settingsRead\":false,\"credentialsRead\":false}\n";return failures?1:0;}
