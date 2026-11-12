// Test TU only: actual protocol/overlapped transport over fresh own-process
// pipes. The production verifier is unchanged; synthetic receipts below are
// neither SYSTEM/admin proof nor a production permissive callback.
#include "signin_diagnostic_inlet.cpp"
#include "json_object.hpp"
#include <atomic>
#include <iostream>
#include <thread>

namespace host::signin::diagnostic {
struct TestAccess {
  static VerifiedOperator own(HANDLE pipe,HANDLE stop,const std::shared_ptr<std::atomic<bool>>& alive,uint64_t ttl=30000){
    auto state=std::make_shared<VerifiedOperator::State>();
    state->process=WinHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,GetCurrentProcessId()));
    state->operatorIdentity=processIdentity(state->process.value);state->serviceIdentity=state->operatorIdentity;
    state->pipe=pipe;state->stop=stop;state->begun=GetTickCount64();state->deadline=state->begun+ttl;
    if(BCryptGenRandom(nullptr,state->nonce.data(),DWORD(state->nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Own challenge unavailable");
    const std::weak_ptr<VerifiedOperator::State> weak=state;
    state->check=[weak,alive]{const auto held=weak.lock();return held&&alive->load()&&
      processIdentity(held->process.value)==held->operatorIdentity&&WaitForSingleObject(held->process.value,0)==WAIT_TIMEOUT;};
    return VerifiedOperator(state);
  }
  static bool converse(HANDLE pipe,HANDLE stop,VerifiedOperator peer,const Server::Handler& handler){
    return Server::converse(pipe,stop,std::move(peer),handler);
  }
  static bool productionRejects(HANDLE pipe,HANDLE stop){
    Server server(processIdentity(GetCurrentProcess()),ToolPin{});return !server.verify(pipe,stop);
  }
};
}

namespace {
using namespace host;
namespace inlet=host::signin::diagnostic;
std::atomic<size_t> checks=0;
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
std::string request(const std::string& nonce){
  return "{\"type\":\"signin.diagnostic.prepare\",\"protocol\":\"signin-diagnostic-inlet-v1\",\"purpose\":\"preparation-only\",\"nonce\":"+
    jsonQuote(nonce)+",\"confirmed\":true,\"width\":1280,\"height\":800}";
}
std::string ownSid(){
  HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw std::runtime_error("Own token unavailable");WinHandle token(raw);
  DWORD bytes=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);std::vector<uint8_t> data(bytes);
  if(!bytes||!GetTokenInformation(token.value,TokenUser,data.data(),bytes,&bytes))throw std::runtime_error("Own SID unavailable");
  LPWSTR text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text))throw std::runtime_error("Own SID format unavailable");
  const auto value=utf8(text);LocalFree(text);return value;
}
struct OwnPipe {
  WinHandle server,client,stop,done;std::thread worker;bool completed=false;
  std::shared_ptr<std::atomic<bool>> alive=std::make_shared<std::atomic<bool>>(true);
  std::wstring name;
  OwnPipe(){
    name=L"\\\\.\\pipe\\Multitap.Test.SigninDiagnostic."+std::to_wstring(GetCurrentProcessId())+L'.'+randomName();
    PSECURITY_DESCRIPTOR descriptor=nullptr;const auto sddl=L"D:P(A;;GA;;;"+wide(ownSid())+L")";
    expect(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Own pipe ACL unavailable");
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    server=WinHandle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));LocalFree(descriptor);
    expect(server.value!=INVALID_HANDLE_VALUE,"Fresh own pipe required");
    stop=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));done=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    expect(stop.value&&done.value,"Own events unavailable");
    WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED pending{};pending.hEvent=event.value;
    const BOOL connected=ConnectNamedPipe(server.value,&pending);const DWORD code=GetLastError();
    expect(connected||code==ERROR_IO_PENDING,"Own connect did not arm");
    client=WinHandle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    expect(client.value!=INVALID_HANDLE_VALUE,"Own client unavailable");
    if(!connected){DWORD count=0;expect(WaitForSingleObject(event.value,2000)==WAIT_OBJECT_0&&GetOverlappedResult(server.value,&pending,&count,FALSE),"Own connect incomplete");}
  }
  ~OwnPipe(){
    if(stop.value)SetEvent(stop.value);client=WinHandle{};
    if(worker.joinable())worker.join();
    if(server.value&&server.value!=INVALID_HANDLE_VALUE)(void)DisconnectNamedPipe(server.value);
  }
  void start(const inlet::Server::Handler& handler,uint64_t ttl=30000){
    auto peer=inlet::TestAccess::own(server.value,stop.value,alive,ttl);
    worker=std::thread([this,handler,peer=std::move(peer)]()mutable{
      completed=inlet::TestAccess::converse(server.value,stop.value,std::move(peer),handler);SetEvent(done.value);
    });
  }
  Json challenge(){
    const auto begun=GetTickCount64();const auto value=inlet::receive(client.value,stop.value,begun,begun+3000);
    expect(value.has_value(),"Own challenge not received");const auto message=JsonParser::parse(*value);
    expect(message.get("type").string()=="signin.diagnostic.challenge"&&message.get("protocol").string()==inlet::protocol&&
      message.get("nonce").string().size()==64,"Challenge protocol malformed");return message;
  }
  void submit(const std::string& bytes){const auto begun=GetTickCount64();expect(inlet::send(client.value,stop.value,bytes,begun,begun+3000),"Own request write failed");}
  std::optional<Json> response(){
    const auto begun=GetTickCount64();const auto bytes=inlet::receive(client.value,stop.value,begun,begun+3000);
    return bytes?std::optional(JsonParser::parse(*bytes)):std::nullopt;
  }
  void finish(){expect(WaitForSingleObject(done.value,4000)==WAIT_OBJECT_0,"Own conversation did not terminate within its bound");worker.join();}
};
void parsing(){
  host::signin::Nonce nonce{};nonce[0]=7;const auto text=inlet::hex(nonce),packet=request(text);
  expect(inlet::parseRequest(packet,nonce).has_value(),"Valid exact preparation request refused");
  for(const auto& key:std::array<const char*,7>{"type","protocol","purpose","nonce","confirmed","width","height"}){
    auto value=JsonParser::parse(packet);std::get<Json::Object>(value.value).erase(key);
    expect(!inlet::parseRequest(jsonObject(value),nonce),"Missing exact field admitted");
  }
  for(const auto& key:std::array<const char*,7>{"path","command","servicePid","operatorPid","desktopId","flags","runtimeQualified"}){
    auto value=JsonParser::parse(packet);std::get<Json::Object>(value.value)[key].value=true;
    expect(!inlet::parseRequest(jsonObject(value),nonce),"Extra authority/command field admitted");
  }
  for(const auto& pair:std::vector<std::pair<std::string,std::string>>{
      {"type","native.reset.request"},{"protocol","signin-private-preparation-v1"},
      {"purpose","capture"},{"nonce",std::string(64,'0')},{"nonce",std::string(64,'A')}}){
    auto value=JsonParser::parse(packet);std::get<Json::Object>(value.value)[pair.first].value=pair.second;
    expect(!inlet::parseRequest(jsonObject(value),nonce),"Wrong protocol/purpose/challenge admitted");
  }
  for(const double width:std::array<double,7>{0,319,1281,8193,-1,1280.5,4294967295.0}){
    auto value=JsonParser::parse(packet);std::get<Json::Object>(value.value)["width"].value=width;
    expect(!inlet::parseRequest(jsonObject(value),nonce),"Invalid geometry admitted");
  }
  auto value=JsonParser::parse(packet);std::get<Json::Object>(value.value)["confirmed"].value=1.0;
  expect(!inlet::parseRequest(jsonObject(value),nonce),"Numeric confirmation admitted");
  std::get<Json::Object>(value.value)["confirmed"].value=false;
  expect(!inlet::parseRequest(jsonObject(value),nonce),"Unconfirmed request admitted");
  expect(!inlet::parseRequest(packet.substr(0,packet.size()-1)+",\"confirmed\":true}",nonce),"Duplicate field admitted");
  expect(!inlet::parseRequest("{broken",nonce)&&!inlet::parseRequest(std::string(8193,'x'),nonce),"Malformed/overlong frame admitted");
  expect(!inlet::parseRequest(packet,{}),"Zero challenge admitted");
  auto later=nonce;++later[0];expect(!inlet::parseRequest(packet,later),"Prior conversation nonce was reused");
  expect(inlet::endpoint({}).empty()&&inlet::endpoint({77,9007199254740993ull}).ends_with(L"77.9007199254740993"),"Endpoint truncated or admitted invalid identity");
  inlet::VerifiedOperator empty;
  expect(!empty.current()&&!empty.consent()()&&!empty.processHandle(),"Default receipt grants authority");
}
void normalConversation(){
  OwnPipe pipe;int handled=0;std::optional<inlet::VerifiedOperator> retained;std::function<bool()> consent;
  pipe.start([&](inlet::VerifiedOperator&& peer,const inlet::Request& request){
    ++handled;expect(peer.current()&&peer.identity()==inlet::processIdentity(GetCurrentProcess())&&peer.processHandle(),"Own synthetic identity lost");
    expect(request.width==1280&&request.height==800,"Geometry changed at handler boundary");
    consent=peer.consent();retained=std::move(peer);return inlet::Result{true,inlet::Code::prepared};
  });
  const auto challenge=pipe.challenge();pipe.submit(request(challenge.get("nonce").string()));const auto reply=pipe.response();pipe.finish();
  expect(pipe.completed&&handled==1&&reply&&reply->get("ok").boolean()&&reply->get("code").string()=="prepared","Normal diagnostic conversation failed");
  expect(reply->get("type").string()=="signin.diagnostic.result"&&reply->get("protocol").string()==inlet::protocol&&
    reply->get("nonce").string()==challenge.get("nonce").string(),"Result correlation changed");
  expect(retained&&!retained->current()&&consent&&!consent(),"Finished conversation retained consent");
  expect(inlet::TestAccess::productionRejects(pipe.server.value,pipe.stop.value),"Own uninstalled process passed production installed-SYSTEM admission");
}
void refusals(){
  for(const bool throws:std::array<bool,2>{false,true}){
    OwnPipe pipe;pipe.start([throws](inlet::VerifiedOperator&&,const inlet::Request&)->inlet::Result{
      if(throws)throw std::runtime_error("PRIVATE_EXCEPTION_MUST_NOT_LEAK");return {false,inlet::Code::busy};
    });
    const auto challenge=pipe.challenge();pipe.submit(request(challenge.get("nonce").string()));const auto reply=pipe.response();pipe.finish();
    expect(reply&&!reply->get("ok").boolean()&&reply->get("code").string()==(throws?"failed":"busy"),"Failure result changed meaning");
    expect(jsonObject(*reply).find("PRIVATE_EXCEPTION")==std::string::npos,"Exception leaked through operator response");
  }
  {OwnPipe pipe;int handled=0;pipe.start([&](auto&&,const auto&){++handled;return inlet::Result{};});
    const auto challenge=pipe.challenge();(void)challenge;pipe.submit(request(std::string(64,'0')));pipe.finish();
    expect(!pipe.completed&&!handled,"Wrong nonce reached handler");}
  {OwnPipe pipe;int handled=0;pipe.start([&](auto&&,const auto&){++handled;return inlet::Result{};});
    const auto challenge=pipe.challenge();pipe.alive->store(false);pipe.submit(request(challenge.get("nonce").string()));pipe.finish();
    expect(!pipe.completed&&!handled,"Retired operator reached handler");}
  {OwnPipe pipe;int handled=0;pipe.start([&](auto&&,const auto&){++handled;return inlet::Result{};});
    const auto challenge=pipe.challenge();const auto packet=request(challenge.get("nonce").string());const DWORD length=DWORD(packet.size());
    std::string both(sizeof(length),'\0');std::memcpy(both.data(),&length,sizeof(length));both+=packet;
    both.append(reinterpret_cast<const char*>(&length),sizeof(length));both+=packet;const auto begun=GetTickCount64();
    expect(inlet::transfer(pipe.client.value,pipe.stop.value,true,both.data(),DWORD(both.size()),begun,begun+3000),"Own replay fixture send failed");
    pipe.finish();expect(!pipe.completed&&!handled,"Queued second frame reused one-shot consent");}
}
void boundedLifetime(){
  {OwnPipe pipe;int handled=0;pipe.start([&](auto&&,const auto&){++handled;return inlet::Result{};},350);
    (void)pipe.challenge();const auto begun=GetTickCount64();pipe.finish();
    expect(!pipe.completed&&!handled&&GetTickCount64()-begun<2000,"No-request deadline was renewed or ignored");}
  {OwnPipe pipe;int handled=0;pipe.start([&](auto&&,const auto&){++handled;return inlet::Result{};});
    (void)pipe.challenge();const auto begun=GetTickCount64();SetEvent(pipe.stop.value);pipe.finish();
    expect(!pipe.completed&&!handled&&GetTickCount64()-begun<2000,"Stop did not cancel pending read");}
  {OwnPipe pipe;int handled=0;pipe.start([&](auto&&,const auto&){++handled;return inlet::Result{};});
    (void)pipe.challenge();pipe.client=WinHandle{};pipe.finish();
    expect(!pipe.completed&&!handled,"Disconnected operator reached handler");}
  {OwnPipe pipe;WinHandle entered(CreateEventW(nullptr,TRUE,FALSE,nullptr)),proceed(CreateEventW(nullptr,TRUE,FALSE,nullptr));bool stale=false;
    pipe.start([&](inlet::VerifiedOperator&& peer,const auto&){SetEvent(entered.value);WaitForSingleObject(proceed.value,2000);stale=!peer.current();return inlet::Result{true,inlet::Code::prepared};});
    const auto challenge=pipe.challenge();pipe.submit(request(challenge.get("nonce").string()));
    expect(WaitForSingleObject(entered.value,2000)==WAIT_OBJECT_0,"Handler did not begin");pipe.client=WinHandle{};SetEvent(proceed.value);pipe.finish();
    expect(stale&&!pipe.completed,"Disconnect did not revoke a moved handler receipt");}
  {OwnPipe pipe;pipe.start([](inlet::VerifiedOperator&&,const auto&){return inlet::Result{true,inlet::Code::unsupported};});
    const auto challenge=pipe.challenge();pipe.submit(request(challenge.get("nonce").string()));const auto reply=pipe.response();pipe.finish();
    expect(reply&&!reply->get("ok").boolean()&&reply->get("code").string()=="failed","Unknown positive result became success");}
}
std::string metadata(){
  return "{\"schema\":\"multitap-signin-diagnostic-tool\",\"version\":1,\"hostVersion\":\"1.0.14\",\"toolFileVersion\":\"1.0.14.0\",\"toolBytes\":12345,\"toolSHA256\":\""+std::string(64,'a')+"\"}";
}
void metadataParsing(){
  const std::filesystem::path fixed=L"C:\\Fixture\\MultitapHostTool.exe";
  const auto bytes=metadata();const auto parsed=inlet::parseToolPinMetadata(bytes,fixed,"1.0.14");
  expect(parsed&&parsed->path==fixed&&parsed->bytes==12345&&parsed->buildTag=="1.0.14.0"&&parsed->sha256[0]==0xaa,
    "Exact fixed metadata did not produce its pinned tool identity");
  for(const auto* key:{"schema","version","hostVersion","toolFileVersion","toolBytes","toolSHA256"}){
    auto value=JsonParser::parse(bytes);std::get<Json::Object>(value.value).erase(key);
    expect(!inlet::parseToolPinMetadata(jsonObject(value),fixed,"1.0.14"),"Missing metadata field admitted");
  }
  for(const auto* key:{"path","command","signature","runtimeQualified"}){
    auto value=JsonParser::parse(bytes);std::get<Json::Object>(value.value)[key].value=std::string("untrusted");
    expect(!inlet::parseToolPinMetadata(jsonObject(value),fixed,"1.0.14"),"Metadata extra identity field admitted");
  }
  for(const auto& pair:std::vector<std::pair<std::string,std::string>>{
      {"schema","old-schema"},{"hostVersion","1.0.13"},{"toolFileVersion","1.0.14"},
      {"toolFileVersion","1.0.14.1"},{"toolSHA256",std::string(64,'A')},
      {"toolSHA256",std::string(64,'0')},{"toolSHA256",std::string(64,'g')},{"toolSHA256",std::string(63,'a')}}){
    auto value=JsonParser::parse(bytes);std::get<Json::Object>(value.value)[pair.first].value=pair.second;
    expect(!inlet::parseToolPinMetadata(jsonObject(value),fixed,"1.0.14"),"Wrong metadata version/hash admitted");
  }
  for(const double size:std::array<double,5>{0,-1,1.5,134217729,9007199254740992.0}){
    auto value=JsonParser::parse(bytes);std::get<Json::Object>(value.value)["toolBytes"].value=size;
    expect(!inlet::parseToolPinMetadata(jsonObject(value),fixed,"1.0.14"),"Invalid metadata file length admitted");
  }
  for(const auto& version:std::array<std::string,5>{"1.0.13","1.0.14+5","01.0.14","1.0","65536.0.0"})
    expect(!inlet::parseToolPinMetadata(bytes,fixed,version),"Nonmatching/noncanonical compiled version admitted");
  for(const std::filesystem::path path:{std::filesystem::path(L"relative/MultitapHostTool.exe"),std::filesystem::path(L"C:\\Fixture\\Other.exe"),
      std::filesystem::path(L"C:\\Fixture\\..\\MultitapHostTool.exe"),std::filesystem::path(L"\\\\server\\share\\MultitapHostTool.exe")})
    expect(!inlet::parseToolPinMetadata(bytes,path,"1.0.14"),"Unfixed/remote tool path admitted");
  expect(!inlet::parseToolPinMetadata(bytes.substr(0,bytes.size()-1)+",\"version\":1}",fixed,"1.0.14"),"Duplicate metadata key admitted");
  expect(!inlet::parseToolPinMetadata("{broken",fixed,"1.0.14")&&!inlet::parseToolPinMetadata(std::string(4097,'x'),fixed,"1.0.14"),"Unbounded/malformed metadata admitted");
  expect(!inlet::loadInstalledToolPin(),"Own uninstalled test process loaded a production allow pin");
}
void metadataFiles(){
  const auto root=std::filesystem::current_path()/L".artifacts"/(L"signin-diagnostic-metadata-"+randomName());
  expect(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Fresh own metadata workspace required");
  const auto trusted=[root](const std::filesystem::path& path){
    const auto relative=path.lexically_relative(root);const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
      (path==root||(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!=L".."));
  };
  const auto write=[&](const wchar_t* leaf,const std::string& bytes){
    const auto path=root/leaf;WinHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    expect(file.value!=INVALID_HANDLE_VALUE,"Own metadata fixture cannot overwrite");DWORD written=0;
    expect(WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file.value),"Own metadata fixture write failed");return path;
  };
  const auto path=write(L"signin-diagnostic-tool.json",metadata());
  expect(inlet::readMetadata(path,trusted)==metadata(),"Held exact bounded metadata read failed");
  expect(!inlet::readMetadata(root/L"missing.json",trusted),"Missing metadata adopted another source");
  expect(!inlet::readMetadata(path,[](const auto&){return false;}),"Untrusted metadata read admitted");
  const auto empty=write(L"empty.json","");expect(!inlet::readMetadata(empty,trusted),"Empty metadata admitted");
  const auto large=write(L"large.json",std::string(4097,'x'));expect(!inlet::readMetadata(large,trusted),"Overlong metadata admitted");
  const auto directory=root/L"directory.json";expect(CreateDirectoryW(directory.c_str(),nullptr)!=FALSE,"Fresh metadata directory fixture failed");
  expect(!inlet::readMetadata(directory,trusted),"Metadata directory admitted");
  {WinHandle writer(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    expect(writer.value!=INVALID_HANDLE_VALUE&&!inlet::readMetadata(path,trusted),"Held writer did not block metadata adoption");}
  expect(inlet::readMetadata(path,trusted)==metadata(),"Released metadata writer poisoned a later normal read");
  expect(CreateHardLinkW((root/L"alias.json").c_str(),path.c_str(),nullptr)!=FALSE,"Own metadata hardlink fixture unavailable");
  expect(!inlet::readMetadata(path,trusted),"Multi-link metadata was accepted");
}
}
int main(int argc,char** argv){
  try{
    const bool metadataOnly=argc==2&&std::string(argv[1])=="--metadata-only";
    if(argc!=1&&!metadataOnly)throw std::runtime_error("Unsupported own-test arguments");
    if(!metadataOnly){parsing();normalConversation();refusals();boundedLifetime();}
    metadataParsing();metadataFiles();
    std::cout<<"{\"kind\":\"signin-diagnostic-own-pipe-fixture\",\"passed\":true,\"checks\":"<<checks.load()
      <<",\"scope\":\""<<(metadataOnly?"metadata-only":"whole-focused")<<"\",\"syntheticOperator\":"<<(metadataOnly?"false":"true")
      <<",\"productionInletRun\":false,\"systemWorkers\":0,\"desktopCalls\":0,\"driverCalls\":0,\"inputEvents\":0,\"runtimeQualified\":false}\n";return 0;
  }catch(const std::exception& error){std::cerr<<"FAILED after "<<checks.load()<<" checks: "<<error.what()<<'\n';return 1;}
}
