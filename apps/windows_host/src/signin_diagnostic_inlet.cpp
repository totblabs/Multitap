#include "signin_diagnostic_inlet.hpp"
#include "broker_status.hpp"
#include "machine.hpp"
#include "owned_output_size.hpp"
#include <bcrypt.h>
#include <winver.h>
#include <mutex>

namespace host::signin::diagnostic {
namespace {
constexpr uint64_t lifetimeMs=30000;
Process processIdentity(HANDLE process){
  return {GetProcessId(process),brokerProcessCreation(process)};
}
bool stopped(HANDLE event){return !event||WaitForSingleObject(event,0)!=WAIT_TIMEOUT;}
bool plainHash(const Nonce& value){return nonzero(value);}
std::string hex(const Nonce& value){
  constexpr char digits[]="0123456789abcdef";std::string text;
  for(const auto byte:value){text+=digits[byte>>4];text+=digits[byte&15];}return text;
}
bool exactFields(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);
  if(!object||object->size()!=names.size())return false;
  return std::all_of(names.begin(),names.end(),[&](const char* name){return object->contains(name);});
}
bool elapsed(uint64_t begun,uint64_t deadline){const auto now=GetTickCount64();return now<begun||now>=deadline;}
bool transfer(HANDLE pipe,HANDLE stopEvent,bool writing,void* buffer,DWORD bytes,uint64_t begun,uint64_t deadline){
  auto* cursor=static_cast<uint8_t*>(buffer);
  while(bytes){
    if(stopped(stopEvent)||elapsed(begun,deadline))return false;
    WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD count=0;
    BOOL ok=writing?WriteFile(pipe,cursor,bytes,&count,&operation):ReadFile(pipe,cursor,bytes,&count,&operation);
    if(!ok&&GetLastError()==ERROR_IO_PENDING){
      const auto now=GetTickCount64();const DWORD remaining=now<deadline?DWORD(std::min<uint64_t>(deadline-now,UINT32_MAX-1)):0;
      HANDLE events[]{event.value,stopEvent};const DWORD wait=WaitForMultipleObjects(2,events,FALSE,remaining);
      if(wait!=WAIT_OBJECT_0){CancelIoEx(pipe,&operation);(void)GetOverlappedResult(pipe,&operation,&count,TRUE);return false;}
      ok=GetOverlappedResult(pipe,&operation,&count,FALSE);
    }
    if(!ok||!count||count>bytes)return false;cursor+=count;bytes-=count;
  }
  return !stopped(stopEvent)&&!elapsed(begun,deadline);
}
bool send(HANDLE pipe,HANDLE stop,const std::string& text,uint64_t begun,uint64_t deadline){
  if(text.empty()||text.size()>8192)return false;
  const auto until=std::min(deadline,GetTickCount64()+3000);DWORD length=DWORD(text.size());auto bytes=text;
  return transfer(pipe,stop,true,&length,sizeof(length),begun,until)&&transfer(pipe,stop,true,bytes.data(),length,begun,until);
}
std::optional<std::string> receive(HANDLE pipe,HANDLE stop,uint64_t begun,uint64_t deadline){
  const auto until=std::min(deadline,GetTickCount64()+2000);DWORD length=0;
  if(!transfer(pipe,stop,false,&length,sizeof(length),begun,until)||!length||length>8192)return {};
  std::string bytes(length,'\0');if(!transfer(pipe,stop,false,bytes.data(),length,begun,until))return {};return bytes;
}
bool connectedPeer(HANDLE pipe,DWORD expected){
  ULONG pid=0;DWORD available=0;
  return GetNamedPipeClientProcessId(pipe,&pid)&&pid==expected&&
    PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)&&available==0;
}
bool elevatedAdmin(HANDLE process,uint32_t session){
  HANDLE raw=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY|TOKEN_DUPLICATE,&raw))return false;WinHandle token(raw);
  TOKEN_ELEVATION elevation{};DWORD count=0,actualSession=UINT32_MAX,appContainer=1;
  if(!GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&count)||!elevation.TokenIsElevated||
      !GetTokenInformation(token.value,TokenSessionId,&actualSession,sizeof(actualSession),&count)||actualSession!=session||
      !GetTokenInformation(token.value,TokenIsAppContainer,&appContainer,sizeof(appContainer),&count)||appContainer)return false;
  SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID admin=nullptr;
  if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&admin))return false;
  struct Sid{PSID value;~Sid(){FreeSid(value);}}sid{admin};
  HANDLE duplicate=nullptr;if(!DuplicateTokenEx(token.value,TOKEN_QUERY,nullptr,SecurityIdentification,TokenImpersonation,&duplicate))return false;
  WinHandle impersonation(duplicate);BOOL member=FALSE;
  return CheckTokenMembership(impersonation.value,admin,&member)&&member;
}
std::vector<uint8_t> operatorSid(HANDLE process){
  HANDLE raw=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&raw))return {};WinHandle token(raw);
  DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
  if(!size||size>16384)return {};std::vector<uint8_t> bytes(size);
  if(!GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size))return {};
  const PSID sid=reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid;
  if(!IsValidSid(sid))return {};std::vector<uint8_t> copied(GetLengthSid(sid));
  if(!CopySid(DWORD(copied.size()),copied.data(),sid))return {};return copied;
}
bool imageHash(HANDLE file,uint64_t expected,const Nonce& wanted){
  LARGE_INTEGER size{},zero{};BY_HANDLE_FILE_INFORMATION info{};
  if(!expected||expected>128*1024*1024||GetFileType(file)!=FILE_TYPE_DISK||
      !GetFileInformationByHandle(file,&info)||info.nNumberOfLinks!=1||
      (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
      !GetFileSizeEx(file,&size)||uint64_t(size.QuadPart)!=expected||!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return false;
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<uint8_t> storage;
  struct Cleanup{BCRYPT_ALG_HANDLE& algorithm;BCRYPT_HASH_HANDLE& hash;~Cleanup(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}}cleanup{algorithm,hash};
  DWORD objectSize=0,count=0;
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
      BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&count,0)<0)return false;
  storage.resize(objectSize);
  if(BCryptCreateHash(algorithm,&hash,storage.data(),objectSize,nullptr,0,0)<0)return false;
  std::array<uint8_t,65536> buffer{};uint64_t total=0;
  for(;;){DWORD read=0;if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&read,nullptr))return false;if(!read)break;
    total+=read;if(total>expected||BCryptHashData(hash,buffer.data(),read,0)<0)return false;}
  Nonce digest{};return total==expected&&BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0)>=0&&digest==wanted;
}
bool imageVersion(const std::filesystem::path& path,const std::string& expected){
  if(expected.empty()||expected.size()>48)return false;DWORD ignored=0;
  const DWORD size=GetFileVersionInfoSizeW(path.c_str(),&ignored);if(!size||size>1024*1024)return false;
  std::vector<uint8_t> bytes(size);VS_FIXEDFILEINFO* value=nullptr;UINT length=0;
  if(!GetFileVersionInfoW(path.c_str(),0,size,bytes.data())||!VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&value),&length)||
      length<sizeof(*value)||value->dwSignature!=0xfeef04bd)return false;
  const auto actual=std::to_string(HIWORD(value->dwFileVersionMS))+'.'+std::to_string(LOWORD(value->dwFileVersionMS))+'.'+
    std::to_string(HIWORD(value->dwFileVersionLS))+'.'+std::to_string(LOWORD(value->dwFileVersionLS));
  return actual==expected;
}
bool heldPath(HANDLE file,const std::filesystem::path& expected){
  std::wstring final(32768,L'\0');const DWORD count=GetFinalPathNameByHandleW(file,final.data(),DWORD(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  if(!count||count>=final.size())return false;final.resize(count);
  return _wcsicmp(final.c_str(),(L"\\\\?\\"+expected.wstring()).c_str())==0;
}
std::optional<std::string> readMetadata(const std::filesystem::path& path,
    const std::function<bool(const std::filesystem::path&)>& trusted){
  try{
    if(!trusted||!trusted(path.parent_path())||!trusted(path))return {};
    WinHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(file.value==INVALID_HANDLE_VALUE||GetFileType(file.value)!=FILE_TYPE_DISK||
        !GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||
        !GetFileSizeEx(file.value,&size)||size.QuadPart<=0||size.QuadPart>4096||
        !heldPath(file.value,path)||!trusted(path))return {};
    std::string bytes(size_t(size.QuadPart),'\0');DWORD read=0;
    if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())return {};
    char extra=0;if(!ReadFile(file.value,&extra,1,&read,nullptr)||read)return {};return bytes;
  }catch(...){return {};}
}
bool hostVersion(const std::string& version){
  if(version.empty()||version.size()>17)return false;size_t begin=0;unsigned parts=0;
  for(size_t index=0;index<=version.size();++index){
    if(index<version.size()&&version[index]!='.')continue;
    const auto part=version.substr(begin,index-begin);if(part.empty()||part.size()>5||(part.size()>1&&part.front()=='0'))return false;
    uint32_t value=0;for(const auto digit:part){if(digit<'0'||digit>'9')return false;value=value*10+uint32_t(digit-'0');}
    if(value>65535)return false;++parts;begin=index+1;
  }
  return parts==3;
}
std::optional<std::filesystem::path> installedService(Process expected){
  if(expected!=processIdentity(GetCurrentProcess())||!brokerSystemProcess(GetCurrentProcess()))return {};
  ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));
  if(!manager.value)return {};ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG));
  if(!service.value)return {};SERVICE_STATUS_PROCESS status{};DWORD count=0;
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&count)||
      status.dwCurrentState!=SERVICE_RUNNING||status.dwProcessId!=expected.pid)return {};
  const auto path=brokerServiceExecutable(service.value);
  if(!brokerProcessExecutable(GetCurrentProcess(),path))return {};return std::filesystem::path(path);
}
const char* codeName(Code value){
  switch(value){case Code::prepared:return "prepared";case Code::busy:return "busy";case Code::expired:return "expired";
    case Code::cancelled:return "cancelled";case Code::failed:return "failed";case Code::restorePending:return "restore_pending";
    case Code::unsupported:return "unsupported";default:return "refused";}
}
}
struct VerifiedOperator::State {
  mutable std::mutex mutex;WinHandle process,image;HANDLE pipe=nullptr,stop=nullptr;
  Process operatorIdentity{},serviceIdentity{};uint32_t console=0;Nonce nonce{};
  std::vector<uint8_t> sid;
  uint64_t begun=0,deadline=0;bool active=true;std::function<bool()> check;
  bool current()const{
    std::lock_guard lock(mutex);
    try{return active&&pipe&&check&&!stopped(stop)&&!elapsed(begun,deadline)&&check()&&connectedPeer(pipe,operatorIdentity.pid);}catch(...){return false;}
  }
};
bool VerifiedOperator::current()const{return state_&&state_->current();}
Process VerifiedOperator::identity()const{return state_?state_->operatorIdentity:Process{};}
HANDLE VerifiedOperator::processHandle()const{return state_?state_->process.value:nullptr;}
uint32_t VerifiedOperator::consoleSession()const{return state_?state_->console:0;}
Nonce VerifiedOperator::operation()const{return state_?state_->nonce:Nonce{};}
uint64_t VerifiedOperator::issuedTick()const{return state_?state_->begun:0;}
uint64_t VerifiedOperator::deadlineTick()const{return state_?state_->deadline:0;}
std::function<bool()> VerifiedOperator::consent()const{const auto state=state_;return [state]{return state&&state->current();};}
std::wstring endpoint(Process service){
  if(!valid(service))return {};return L"\\\\.\\pipe\\MultitapHost.SigninDiagnostic."+std::to_wstring(service.pid)+L'.'+std::to_wstring(service.created);
}
std::optional<Request> parseRequest(const std::string& bytes,const Nonce& challenge){
  try{
    if(bytes.empty()||bytes.size()>8192||!plainHash(challenge))return {};const auto value=JsonParser::parse(bytes);
    if(!exactFields(value,{"type","protocol","purpose","nonce","confirmed","width","height"})||
        value.get("type").string()!="signin.diagnostic.prepare"||value.get("protocol").string()!=protocol||
        value.get("purpose").string()!="preparation-only"||value.get("nonce").string()!=hex(challenge)||
        !std::holds_alternative<bool>(value.get("confirmed").value)||!value.get("confirmed").boolean())return {};
    const auto width=value.get("width").number(-1),height=value.get("height").number(-1);
    if(!std::isfinite(width)||!std::isfinite(height)||std::floor(width)!=width||std::floor(height)!=height||
        width<320||width>8192||height<180||height>8192||
        !ownedOutputSize(DWORD(width),DWORD(height)))return {};
    return Request{uint32_t(width),uint32_t(height)};
  }catch(...){return {};}
}
std::optional<ToolPin> parseToolPinMetadata(const std::string& bytes,
    const std::filesystem::path& fixedSibling,const std::string& compiledHostVersion){
  try{
    const auto native=fixedSibling.native();
    if(bytes.empty()||bytes.size()>4096||!hostVersion(compiledHostVersion)||!fixedSibling.is_absolute()||
        fixedSibling.lexically_normal()!=fixedSibling||native.size()<4||native[1]!=L':'||native[2]!=L'\\'||
        native.find(L'\0')!=std::wstring::npos||_wcsicmp(fixedSibling.filename().c_str(),L"MultitapHostTool.exe"))return {};
    const auto value=JsonParser::parse(bytes);
    if(!exactFields(value,{"schema","version","hostVersion","toolFileVersion","toolBytes","toolSHA256"})||
        value.get("schema").string()!="multitap-signin-diagnostic-tool"||value.get("version").number()!=1||
        value.get("hostVersion").string()!=compiledHostVersion||
        value.get("toolFileVersion").string()!=compiledHostVersion+".0")return {};
    const auto size=value.get("toolBytes").number(-1);const auto sha=value.get("toolSHA256").string();
    if(!std::isfinite(size)||std::floor(size)!=size||size<1||size>128*1024*1024||sha.size()!=64||
        !std::all_of(sha.begin(),sha.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))return {};
    ToolPin pin;pin.path=fixedSibling;pin.bytes=uint64_t(size);pin.buildTag=value.get("toolFileVersion").string();
    const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
    for(size_t index=0;index<pin.sha256.size();++index)pin.sha256[index]=uint8_t((digit(sha[index*2])<<4)|digit(sha[index*2+1]));
    if(!nonzero(pin.sha256))return {};return pin;
  }catch(...){return {};}
}
std::optional<ToolPin> loadInstalledToolPin(){
  try{
    const auto service=installedService(processIdentity(GetCurrentProcess()));if(!service)return {};
    const auto bin=service->parent_path();const auto bytes=readMetadata(bin/L"signin-diagnostic-tool.json",trustedMachineFile);
    if(!bytes)return {};return parseToolPinMetadata(*bytes,bin/L"MultitapHostTool.exe",MULTITAP_HOST_VERSION_UTF8);
  }catch(...){return {};}
}
std::optional<VerifiedOperator> Server::verify(HANDLE pipe,HANDLE stopEvent)const{
  try{
    const auto service=installedService(service_);if(!service||!plainHash(tool_.sha256)||
        !tool_.path.is_absolute()||tool_.path.lexically_normal()!=tool_.path||
        _wcsicmp(tool_.path.c_str(),(*service).parent_path().append(L"MultitapHostTool.exe").c_str())!=0)return {};
    ULONG pid=0;if(!GetNamedPipeClientProcessId(pipe,&pid)||!pid||pid==service_.pid)return {};
    auto state=std::make_shared<VerifiedOperator::State>();state->begun=GetTickCount64();state->deadline=state->begun+lifetimeMs;
    state->process=WinHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
    if(!state->process.value||WaitForSingleObject(state->process.value,0)!=WAIT_TIMEOUT)return {};
    state->operatorIdentity=processIdentity(state->process.value);state->serviceIdentity=service_;
    DWORD session=UINT32_MAX;if(!valid(state->operatorIdentity)||!ProcessIdToSessionId(pid,&session)||!session||session==UINT32_MAX||
        session!=WTSGetActiveConsoleSessionId()||!brokerProcessExecutable(state->process.value,tool_.path.wstring())||
        !elevatedAdmin(state->process.value,session))return {};
    state->sid=operatorSid(state->process.value);if(state->sid.empty())return {};
    if(!trustedMachineFile(tool_.path)||!trustedMachineFile(tool_.path.parent_path()))return {};
    state->image=WinHandle(CreateFileW(tool_.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(state->image.value==INVALID_HANDLE_VALUE||!heldPath(state->image.value,tool_.path)||
        !imageHash(state->image.value,tool_.bytes,tool_.sha256)||!imageVersion(tool_.path,tool_.buildTag))return {};
    state->pipe=pipe;state->stop=stopEvent;state->console=session;
    if(BCryptGenRandom(nullptr,state->nonce.data(),DWORD(state->nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0||!nonzero(state->nonce))return {};
    const std::weak_ptr<VerifiedOperator::State> weak=state;
    const auto expected=tool_.path;const auto serviceIdentity=service_;
    state->check=[weak,expected,serviceIdentity]{const auto held=weak.lock();return held&&
      processIdentity(GetCurrentProcess())==serviceIdentity&&processIdentity(held->process.value)==held->operatorIdentity&&
      WaitForSingleObject(held->process.value,0)==WAIT_TIMEOUT&&held->console==WTSGetActiveConsoleSessionId()&&
      brokerProcessExecutable(held->process.value,expected.wstring())&&elevatedAdmin(held->process.value,held->console)&&
      operatorSid(held->process.value)==held->sid;};
    VerifiedOperator result(state);if(!result.current())return {};return result;
  }catch(...){return {};}
}
bool Server::converse(HANDLE pipe,HANDLE stopEvent,VerifiedOperator peer,const Handler& handler){
  const auto state=peer.state_;if(!state||!handler)return false;
  struct Retire {std::shared_ptr<VerifiedOperator::State> state;~Retire(){std::lock_guard lock(state->mutex);state->active=false;state->pipe=nullptr;}}retire{state};
  try{
    if(!peer.current())return false;
    const auto challenge="{\"type\":\"signin.diagnostic.challenge\",\"protocol\":"+jsonQuote(protocol)+",\"nonce\":"+jsonQuote(hex(state->nonce))+",\"timeoutMs\":30000}";
    if(!send(pipe,stopEvent,challenge,state->begun,state->deadline))return false;
    const auto frame=receive(pipe,stopEvent,state->begun,state->deadline);if(!frame)return false;
    const auto request=parseRequest(*frame,state->nonce);if(!request||!peer.current())return false;
    auto consent=peer.consent();Result result;
    try{result=handler(std::move(peer),*request);}catch(...){result={false,Code::failed};}
    if(!consent())return false;
    if(result.ok&&result.code!=Code::prepared)result={false,Code::failed};
    const auto reply="{\"type\":\"signin.diagnostic.result\",\"protocol\":"+jsonQuote(protocol)+",\"nonce\":"+jsonQuote(hex(state->nonce))+",\"ok\":"+(result.ok?"true":"false")+",\"code\":"+jsonQuote(codeName(result.code))+'}';
    return send(pipe,stopEvent,reply,state->begun,state->deadline);
  }catch(...){return false;}
}
bool Server::run(HANDLE stopEvent,const Handler& handler){
  if(!handler||stopped(stopEvent)||!valid(service_)||endpoint(service_).empty())return false;
  try{
    if(!installedService(service_))return false;
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr))return false;
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    WinHandle pipe(CreateNamedPipeW(endpoint(service_).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));LocalFree(descriptor);
    if(pipe.value==INVALID_HANDLE_VALUE)return false;
    while(!stopped(stopEvent)){
      WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;
      OVERLAPPED operation{};operation.hEvent=event.value;BOOL connected=ConnectNamedPipe(pipe.value,&operation);const DWORD error=GetLastError();
      if(!connected&&error==ERROR_IO_PENDING){
        HANDLE events[]{event.value,stopEvent};DWORD count=0;const auto wait=WaitForMultipleObjects(2,events,FALSE,INFINITE);
        if(wait!=WAIT_OBJECT_0){CancelIoEx(pipe.value,&operation);(void)GetOverlappedResult(pipe.value,&operation,&count,TRUE);return stopped(stopEvent);}
        connected=GetOverlappedResult(pipe.value,&operation,&count,FALSE);
      }else if(!connected&&error==ERROR_PIPE_CONNECTED)connected=TRUE;
      if(connected&&!stopped(stopEvent)){if(auto peer=verify(pipe.value,stopEvent))(void)converse(pipe.value,stopEvent,std::move(*peer),handler);}
      (void)DisconnectNamedPipe(pipe.value);
    }
    return true;
  }catch(...){return false;}
}
}
