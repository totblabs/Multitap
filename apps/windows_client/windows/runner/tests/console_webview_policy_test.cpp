#include "console_webview.h"
#include "console_webview_close.h"
#include <iostream>
#include <stdexcept>

namespace {
int checks=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
}
int main(){try{
  const std::string origin="https://vdi.example.test",id="vm-170",code(43,'a');
  require(multitap::ConsoleWebViewOwnerId(code)=="66d34fba71f8f450f7e45598853e53bfc23bbd129027cbb131a2f4ffd7878cd0","Owner correlation hash differed from code SHA256");
  require(multitap::ConsoleWebViewOwnerId(code)!=multitap::ConsoleWebViewOwnerId(std::string(43,'b')),"Different bootstrap owners collided");
  const auto valid=multitap::ConsoleWebViewDestination(origin,id,"/console/"+id+"/attach",code);
  require(valid.has_value(),"Valid scoped bootstrap rejected");
  require(valid->origin==origin&&valid->page==origin+"/console/vm-170"&&valid->attach==origin+"/console/vm-170/attach","Destination widened route");
  require(valid->attach.find(code)==std::string::npos&&valid->page.find(code)==std::string::npos,"Code leaked to navigation URL");
  require(multitap::ConsoleWebViewAttachPost(*valid,valid->attach,"POST",true,true),"Initial owned attach POST was not selected");
  require(!multitap::ConsoleWebViewAttachPost(*valid,valid->attach,"POST",false,true),"A repeated attach POST changed Origin");
  require(!multitap::ConsoleWebViewAttachPost(*valid,valid->attach,"POST",true,false),"A subresource attach POST changed Origin");
  for(const auto* method:{"GET","PUT","post","DELETE"})require(!multitap::ConsoleWebViewAttachPost(*valid,valid->attach,method,true,true),"A different method changed Origin");
  for(const auto* uri:{"https://evil.test/console/vm-170/attach","https://vdi.example.test/console/vm-171/attach","https://vdi.example.test/console/vm-170/attach?x=1","https://vdi.example.test/console/vm-170/attach#x"})
    require(!multitap::ConsoleWebViewAttachPost(*valid,uri,"POST",true,true),"An unrelated attach destination changed Origin");
  auto malformed=*valid;malformed.attach=origin+"/console/vm-171/attach";
  require(!multitap::ConsoleWebViewAttachPost(malformed,malformed.attach,"POST",true,true),"An invalid target binding changed Origin");
  malformed=*valid;malformed.origin+='/';require(!multitap::ConsoleWebViewAttachPost(malformed,malformed.attach,"POST",true,true),"A noncanonical Origin changed the owned POST");
  for(const auto* bad:{"http://vdi.example.test","https://user@vdi.example.test","https://vdi.example.test/x","https://vdi.example.test?x=1","https://vdi.example.test#x","https://vdi.example.test:0","https://vdi.example.test:65536","https://vdi.example.test\r\nOrigin: https://evil.test"})
    require(!multitap::ConsoleWebViewDestination(bad,id,"/console/"+id+"/attach",code),"Unsafe origin accepted");
  for(const auto* bad:{"/console/vm-171/attach","/console/vm-170/attach?code=x","/console/vm-170/attach#x","/console/vm-170/../attach","//evil.test/attach","https://evil.test/console/vm-170/attach"})
    require(!multitap::ConsoleWebViewDestination(origin,id,bad,code),"Wrong attach path accepted");
  for(const auto* bad:{"", "../vm170","vm/170","vm?170","vm#170","vm%2f170","vm 170"})
    require(!multitap::ConsoleWebViewDestination(origin,bad,"/console/"+std::string(bad)+"/attach",code),"Unsafe desktop ID accepted");
  for(const char bad:{' ', '/', '+', '=', '\r', '\n', '?', '#', '%'}){
    auto changed=code;changed[20]=bad;require(!multitap::ConsoleWebViewDestination(origin,id,"/console/"+id+"/attach",changed),"Unsafe code alphabet accepted");
  }
  for(const size_t length:{0u,42u,44u,400u})require(!multitap::ConsoleWebViewDestination(origin,id,"/console/"+id+"/attach",std::string(length,'a')),"Wrong code length accepted");
  require(multitap::ConsoleWebViewNavigation(*valid,valid->attach,true),"Initial POST blocked");
  require(!multitap::ConsoleWebViewNavigation(*valid,valid->attach,false),"Bootstrap endpoint reused as ordinary navigation");
  require(multitap::ConsoleWebViewNavigation(*valid,valid->page,true)&&multitap::ConsoleWebViewNavigation(*valid,valid->page,false),"Original console page blocked");
  for(const auto* bad:{"https://evil.test/console/vm-170","https://vdi.example.test.evil.test/console/vm-170","https://vdi.example.test/console/vm-171","https://vdi.example.test/console/vm-170?code=x","file:///C:/Windows/win.ini","about:blank","data:text/html,x","javascript:alert(1)"})
    require(!multitap::ConsoleWebViewNavigation(*valid,bad,true),"Unexpected top-level destination accepted");
  require(multitap::ConsoleWebViewResource(*valid,origin+"/console/assets/vendor/core/rfb.js"),"Pinned console asset route blocked");
  require(multitap::ConsoleWebViewResource(*valid,origin+"/console/api/session"),"Console authentication route blocked");
  for(const auto* bad:{"https://evil.test/console/assets/x.js","https://vdi.example.test.evil.test/console/assets/x.js","https://vdi.example.test/api/v1/admin","https://vdi.example.test/console/../admin","https://vdi.example.test/console/%2e%2e/admin","https://vdi.example.test/console/assets/x.js?x=y","file:///C:/private.txt"})
    require(!multitap::ConsoleWebViewResource(*valid,bad),"Resource escaped configured console origin/path");
  // The page's own close question: fixed strings only, and never a trap.
  using multitap::ConsolePageMessage;using Step=multitap::ConsoleCloseFlow::Step;
  require(multitap::ParseConsolePageMessage(L"multitap-console:ready")==ConsolePageMessage::Ready&&multitap::ParseConsolePageMessage(L"multitap-console:close-shown")==ConsolePageMessage::Shown&&
    multitap::ParseConsolePageMessage(L"multitap-console:close")==ConsolePageMessage::Confirm&&multitap::ParseConsolePageMessage(L"multitap-console:close-cancel")==ConsolePageMessage::Cancel,"A page answer was not recognised");
  for(const auto* other:{L"",L"multitap-console:",L"multitap-console:close ",L" multitap-console:close",L"MULTITAP-CONSOLE:CLOSE",L"multitap-console:close-request",L"multitap-console:closed",L"{\"type\":\"multitap-console:close\"}"})
    require(multitap::ParseConsolePageMessage(other)==ConsolePageMessage::Ignored,"An unexpected page message was accepted");
  {multitap::ConsoleCloseFlow flow;require(flow.Request()==Step::AskNative,"A page that never said ready was asked");
   require(flow.Message(ConsolePageMessage::Confirm)==Step::None,"An unrequested confirm closed the window");}
  {multitap::ConsoleCloseFlow flow;flow.Message(ConsolePageMessage::Ready);
   require(flow.Message(ConsolePageMessage::Confirm)==Step::None,"A confirm before any request closed the window");
   require(flow.Request()==Step::AskPage,"A ready page was not asked");require(flow.Message(ConsolePageMessage::Shown)==Step::Answered,"The page's answer did not stop the fallback timer");
   require(flow.Message(ConsolePageMessage::Confirm)==Step::Close,"The page's confirm did not close");require(flow.Message(ConsolePageMessage::Confirm)==Step::None,"A repeated confirm acted twice");}
  {multitap::ConsoleCloseFlow flow;flow.Message(ConsolePageMessage::Ready);flow.Request();
   require(flow.Message(ConsolePageMessage::Cancel)==Step::Answered,"Cancel was not taken");require(flow.Message(ConsolePageMessage::Confirm)==Step::None,"A confirm after cancel closed the window");
   require(flow.Unanswered()==Step::None,"A late timer after cancel asked natively");require(flow.Request()==Step::AskPage,"A page that cancelled once was not asked again");}
  {multitap::ConsoleCloseFlow flow;flow.Message(ConsolePageMessage::Ready);flow.Request();
   require(flow.Unanswered()==Step::AskNative,"An unanswered request trapped the user");require(flow.Request()==Step::AskNative,"An unresponsive page was asked again");
   require(flow.Message(ConsolePageMessage::Confirm)==Step::None,"A late page confirm closed after the native question took over");}
  {multitap::ConsoleCloseFlow flow;flow.Message(ConsolePageMessage::Ready);flow.Navigated();require(flow.Request()==Step::AskNative,"A new document inherited readiness");}
  {multitap::ConsoleCloseFlow flow;require(flow.Unanswered()==Step::None,"A stray timer asked without a request");}
  std::cout<<"{\"checks\":"<<checks<<",\"scope\":\"pure console WebView bootstrap/navigation and close-question policy; no browser, UI, network or input\"}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
