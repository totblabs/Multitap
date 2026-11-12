// Test TU: actual client protocol paired with actual inlet conversation over
// isolated Test.* own-process pipes. No production service/admission is run.
#include "signin_diagnostic_client.cpp"
#include "signin_diagnostic_inlet.cpp"
#include "json_object.hpp"
#include <atomic>
#include <iostream>
#include <thread>

namespace host::signin::diagnostic {
struct TestAccess {
  static bool serve(HANDLE pipe,HANDLE stop,const Server::Handler& handler){
    auto state=std::make_shared<VerifiedOperator::State>();
    state->process=WinHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,GetCurrentProcessId()));
    state->operatorIdentity=processIdentity(state->process.value);state->serviceIdentity=state->operatorIdentity;
    state->pipe=pipe;state->stop=stop;state->begun=GetTickCount64();state->deadline=state->begun+30000;
    if(BCryptGenRandom(nullptr,state->nonce.data(),DWORD(state->nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Own test nonce unavailable");
    const std::weak_ptr<VerifiedOperator::State> weak=state;
    state->check=[weak]{const auto value=weak.lock();return value&&processIdentity(value->process.value)==value->operatorIdentity&&
      WaitForSingleObject(value->process.value,0)==WAIT_TIMEOUT;};
    return Server::converse(pipe,stop,VerifiedOperator(state),handler);
  }
};
}
namespace {
using namespace host;
namespace c=host::signin::diagnostic::client;
namespace inlet=host::signin::diagnostic;
std::atomic<size_t> checks=0;
void expect(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);++checks;}
const std::string nonce(64,'a');
std::string offer(){return "{\"type\":\"signin.diagnostic.challenge\",\"protocol\":\"signin-diagnostic-inlet-v1\",\"nonce\":\""+nonce+"\",\"timeoutMs\":30000}";}
std::string result(bool ok=true,const std::string& code="prepared"){
  return "{\"type\":\"signin.diagnostic.result\",\"protocol\":\"signin-diagnostic-inlet-v1\",\"nonce\":\""+nonce+"\",\"ok\":"+(ok?"true":"false")+",\"code\":"+jsonQuote(code)+'}';
}
void parsers(){
  expect(c::challenge(offer()).has_value()&&c::response(result(),nonce)==c::Code::prepared,"Exact client protocol refused");
  for(const auto* name:{"type","protocol","nonce","timeoutMs"}){
    auto value=JsonParser::parse(offer());std::get<Json::Object>(value.value).erase(name);
    expect(!c::challenge(jsonObject(value)),"Incomplete challenge admitted");
  }
  for(const auto* name:{"path","flags","servicePid"}){
    auto value=JsonParser::parse(offer());std::get<Json::Object>(value.value)[name].value=true;
    expect(!c::challenge(jsonObject(value)),"Unexpected challenge authority admitted");
  }
  for(const auto& text:{std::string(64,'0'),std::string(64,'A'),std::string(63,'a')}){
    auto value=JsonParser::parse(offer());std::get<Json::Object>(value.value)["nonce"].value=text;
    expect(!c::challenge(jsonObject(value)),"Noncanonical/zero challenge admitted");
  }
  for(const double time:std::array<double,5>{0,-1,30001,1.5,4294967295.0}){
    auto value=JsonParser::parse(offer());std::get<Json::Object>(value.value)["timeoutMs"].value=time;
    expect(!c::challenge(jsonObject(value)),"Invalid challenge deadline admitted");
  }
  expect(!c::challenge("{\"type\":\"native.reset.console\"}")&&!c::challenge(offer().substr(0,offer().size()-1)+",\"nonce\":\""+nonce+"\"}"),"Old reset or duplicate fields admitted");
  for(const auto* name:{"type","protocol","nonce","ok","code"}){
    auto value=JsonParser::parse(result());std::get<Json::Object>(value.value).erase(name);
    expect(!c::response(jsonObject(value),nonce),"Incomplete final result admitted");
  }
  expect(!c::response(result(),std::string(64,'b'))&&!c::response(result(true,"unknown"),nonce)&&
    !c::response(result(false,"prepared"),nonce)&&!c::response(result(true,"failed"),nonce),"Unbound/unknown/inconsistent ACK became success");
  for(const auto& code:{"refused","busy","expired","cancelled","failed","restore_pending","unsupported"})
    expect(c::response(result(false,code),nonce).has_value(),"Fixed negative result lost");
  expect(!c::response(result().substr(0,result().size()-1)+",\"password\":\"MUST_NOT_ADMIT\"}",nonce),"Extra result content admitted");
  expect(!c::challenge(std::string(8193,'x'))&&!c::response("{broken",nonce),"Malformed/unbounded result admitted");
  const wchar_t* valid[]{L"C:\\Program Files\\Multitap\\Host\\1.0.14\\bin\\MultitapHostTool.exe",L"--signin-diagnostic-prepare-v1",L"--confirmed",L"1280",L"800"};
  expect(c::parseCommand(5,valid).has_value(),"Fixed explicit CLI intent refused");
  for(int index=0;index<5;++index){auto args=std::array<const wchar_t*,5>{valid[0],valid[1],valid[2],valid[3],valid[4]};args[size_t(index)]=L"--unexpected";
    expect(!c::parseCommand(5,args.data()),"Wrong CLI image/confirmation/geometry admitted");}
  expect(!c::parseCommand(4,valid)&&!c::parseCommand(6,valid)&&!c::parseCommand(5,nullptr),"Malformed CLI arity admitted");
  expect(c::requestPreparation(0,800).code==c::Code::refused&&!c::requestPreparation(1280,800).confirmationAttempted,
    "Own uninstalled test executable entered production preparation");
}
struct Pipe {
  WinHandle server,client,stop,done;std::thread worker;std::atomic<bool> live=true;bool serverDone=false;
  Pipe(){
    HANDLE raw=nullptr;expect(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)!=FALSE,"Own token unavailable");WinHandle token(raw);
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<uint8_t> bytes(size);
    expect(size&&GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size),"Own SID unavailable");LPWSTR text=nullptr;
    expect(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text)!=FALSE,"Own SID conversion unavailable");
    const auto sddl=L"D:P(A;;GA;;;"+std::wstring(text)+L")";LocalFree(text);PSECURITY_DESCRIPTOR descriptor=nullptr;
    expect(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Own ACL unavailable");
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};const auto name=L"\\\\.\\pipe\\Multitap.Test.SigninClient."+std::to_wstring(GetCurrentProcessId())+L'.'+randomName();
    server=WinHandle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));LocalFree(descriptor);
    expect(server.value!=INVALID_HANDLE_VALUE,"Fresh own pipe unavailable");stop=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));done=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    expect(stop.value&&done.value,"Own stop/done unavailable");WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED pending{};pending.hEvent=event.value;
    const BOOL connected=ConnectNamedPipe(server.value,&pending);const auto error=GetLastError();expect(connected||error==ERROR_IO_PENDING,"Own connect did not arm");
    client=WinHandle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));expect(client.value!=INVALID_HANDLE_VALUE,"Own client unavailable");
    if(!connected){DWORD count=0;expect(WaitForSingleObject(event.value,2000)==WAIT_OBJECT_0&&GetOverlappedResult(server.value,&pending,&count,FALSE),"Own connect incomplete");}
  }
  ~Pipe(){if(stop.value)SetEvent(stop.value);client=WinHandle{};if(worker.joinable())worker.join();if(server.value&&server.value!=INVALID_HANDLE_VALUE)DisconnectNamedPipe(server.value);}
  void start(const std::function<void()>& work){worker=std::thread([this,work]{work();SetEvent(done.value);});}
  c::Result call(const std::function<bool()>& current={},uint64_t ttl=30000,host::signin::Process expected={}){
    const auto begun=GetTickCount64();if(!host::signin::valid(expected))expected=c::identity(GetCurrentProcess());
    return c::converse(client.value,stop.value,expected,1280,800,begun,begun+ttl,current?current:[this]{return live.load();});
  }
  void finish(){client=WinHandle{};expect(WaitForSingleObject(done.value,4000)==WAIT_OBJECT_0,"Own peer failed to finish");worker.join();}
  bool send(const std::string& bytes){const auto begun=GetTickCount64();return c::writeFrame(server.value,stop.value,bytes,begun,begun+3000);}
  std::optional<std::string> receive(){const auto begun=GetTickCount64();return c::readFrame(server.value,stop.value,begun,begun+2000,2000);}
};
void paired(){
  {Pipe pipe;int handled=0;pipe.start([&]{pipe.serverDone=inlet::TestAccess::serve(pipe.server.value,pipe.stop.value,[&](auto&& peer,const auto& request){
      ++handled;expect(peer.current()&&request.width==1280&&request.height==800,"Actual paired request/owner changed");return inlet::Result{true,inlet::Code::prepared};});});
    const auto reply=pipe.call();pipe.finish();expect(reply.code==c::Code::prepared&&reply.confirmationAttempted&&reply.observedResult&&handled==1&&pipe.serverDone,
      "Actual paired client/inlet failed or dispatched more than once");}
  {Pipe pipe;int handled=0;pipe.start([&]{pipe.serverDone=inlet::TestAccess::serve(pipe.server.value,pipe.stop.value,[&](auto&&,const auto&){++handled;pipe.live=false;return inlet::Result{true,inlet::Code::prepared};});});
    const auto reply=pipe.call();pipe.finish();expect(reply.code==c::Code::unverified&&reply.confirmationAttempted&&!reply.observedResult&&handled==1,
      "Retired service owner adopted a late real ACK");}
  {Pipe pipe;int guard=0,handled=0;pipe.start([&]{pipe.serverDone=inlet::TestAccess::serve(pipe.server.value,pipe.stop.value,[&](auto&&,const auto&){++handled;return inlet::Result{};});});
    const auto reply=pipe.call([&]{return ++guard==1;});pipe.finish();expect(!reply.confirmationAttempted&&!handled,"Changed service before confirmation reached handler");}
}
void adversarial(){
  {Pipe pipe;pipe.start([&]{pipe.send(offer());});auto wrong=c::identity(GetCurrentProcess());++wrong.pid;
    const auto reply=pipe.call({},30000,wrong);pipe.finish();expect(!reply.confirmationAttempted&&!reply.observedResult,"Wrong kernel server PID admitted confirmation");}
  for(const auto& challenge:{std::string("{\"type\":\"native.reset.console\"}"),std::string("{broken")}){
    Pipe pipe;pipe.start([&]{pipe.send(challenge);});const auto reply=pipe.call();pipe.finish();
    expect(reply.code==c::Code::unsupported&&!reply.confirmationAttempted,"Old/malformed challenge triggered confirmation or reset fallback");
  }
  for(const auto& final:{result(true,"unknown"),result(false,"prepared"),result().replace(result().find(nonce),nonce.size(),std::string(64,'b'))}){
    Pipe pipe;int received=0;pipe.start([&]{if(pipe.send(offer())&&pipe.receive()){++received;pipe.send(final);}});
    const auto reply=pipe.call();pipe.finish();expect(reply.code==c::Code::unverified&&reply.confirmationAttempted&&!reply.observedResult&&received==1,
      "Bad final ACK succeeded or caused automatic replay");
  }
  {Pipe pipe;int received=0;pipe.start([&]{if(pipe.send(offer())&&pipe.receive())++received;DisconnectNamedPipe(pipe.server.value);});
    const auto reply=pipe.call();pipe.finish();expect(reply.code==c::Code::unverified&&reply.confirmationAttempted&&!reply.observedResult&&received==1,"Closed peer after request was retried or reported unmutated");}
  {Pipe pipe;pipe.start([&]{WaitForSingleObject(pipe.stop.value,2000);});const auto begun=GetTickCount64();const auto reply=pipe.call({},150);SetEvent(pipe.stop.value);pipe.finish();
    expect(!reply.confirmationAttempted&&GetTickCount64()-begun<2000,"Missing challenge extended finite client wait");}
  {Pipe pipe;pipe.start([&]{SetEvent(pipe.stop.value);});const auto reply=pipe.call();pipe.finish();expect(!reply.confirmationAttempted,"Cancelled client still confirmed");}
  {Pipe pipe;int received=0;pipe.start([&]{if(pipe.send(offer())&&pipe.receive()){++received;pipe.live=false;pipe.send(result());}});
    const auto reply=pipe.call();pipe.finish();expect(reply.code==c::Code::unverified&&!reply.observedResult&&received==1,"Stale-generation callback accepted late ACK");}
}
}
int main(){
  try{parsers();paired();adversarial();std::cout<<"{\"kind\":\"signin-diagnostic-client-own-pair\",\"passed\":true,\"checks\":"<<checks.load()
    <<",\"syntheticPeer\":true,\"productionAdmission\":false,\"actualService\":false,\"systemWorkers\":0,\"desktopCalls\":0,\"driverCalls\":0,\"inputEvents\":0,\"runtimeQualified\":false}\n";return 0;}
  catch(const std::exception& error){std::cerr<<"FAILED after "<<checks.load()<<" checks: "<<error.what()<<'\n';return 1;}
}
