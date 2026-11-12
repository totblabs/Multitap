#include "signin_diagnostic_client.hpp"
#include "broker_status.hpp"
#include "machine.hpp"
#include "owned_output_size.hpp"
#include <bcrypt.h>
#include <winver.h>

namespace host::signin::diagnostic::client {
namespace {
constexpr uint64_t observationMs=30000;
Process identity(HANDLE process){return {GetProcessId(process),brokerProcessCreation(process)};}
bool cancelled(HANDLE event){return !event||WaitForSingleObject(event,0)!=WAIT_TIMEOUT;}
bool expired(uint64_t begun,uint64_t deadline){const auto now=GetTickCount64();return now<begun||now>=deadline;}
bool fields(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);return object&&object->size()==names.size()&&
    std::all_of(names.begin(),names.end(),[&](const auto* name){return object->contains(name);});
}
bool nonceText(const std::string& text){return text.size()==64&&text.find_first_not_of("0123456789abcdef")==std::string::npos&&text.find_first_not_of('0')!=std::string::npos;}
bool transferBytes(HANDLE pipe,HANDLE stop,bool writing,void* buffer,DWORD bytes,uint64_t begun,uint64_t deadline){
  auto* cursor=static_cast<uint8_t*>(buffer);
  while(bytes){
    if(cancelled(stop)||expired(begun,deadline))return false;
    WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD count=0;
    BOOL ok=writing?WriteFile(pipe,cursor,bytes,&count,&operation):ReadFile(pipe,cursor,bytes,&count,&operation);
    if(!ok&&GetLastError()==ERROR_IO_PENDING){
      const auto now=GetTickCount64();const DWORD remaining=now<deadline?DWORD(std::min<uint64_t>(deadline-now,UINT32_MAX-1)):0;
      HANDLE events[]{event.value,stop};const DWORD wait=WaitForMultipleObjects(2,events,FALSE,remaining);
      if(wait!=WAIT_OBJECT_0){CancelIoEx(pipe,&operation);(void)GetOverlappedResult(pipe,&operation,&count,TRUE);return false;}
      ok=GetOverlappedResult(pipe,&operation,&count,FALSE);
    }
    if(!ok||!count||count>bytes)return false;cursor+=count;bytes-=count;
  }
  return !cancelled(stop)&&!expired(begun,deadline);
}
bool writeFrame(HANDLE pipe,HANDLE stop,const std::string& text,uint64_t begun,uint64_t deadline){
  if(text.empty()||text.size()>8192)return false;DWORD length=DWORD(text.size());auto copy=text;
  const auto until=std::min(deadline,GetTickCount64()+3000);
  return transferBytes(pipe,stop,true,&length,sizeof(length),begun,until)&&transferBytes(pipe,stop,true,copy.data(),length,begun,until);
}
std::optional<std::string> readFrame(HANDLE pipe,HANDLE stop,uint64_t begun,uint64_t deadline,uint64_t frameBudget){
  const auto until=std::min(deadline,GetTickCount64()+frameBudget);DWORD length=0;
  if(!transferBytes(pipe,stop,false,&length,sizeof(length),begun,until)||!length||length>8192)return {};
  std::string bytes(length,'\0');if(!transferBytes(pipe,stop,false,bytes.data(),length,begun,until))return {};return bytes;
}
bool sameServer(HANDLE pipe,Process expected){ULONG pid=0;return valid(expected)&&GetNamedPipeServerProcessId(pipe,&pid)&&pid==expected.pid;}
struct Challenge {std::string nonce;uint32_t timeout=0;};
std::optional<Challenge> challenge(const std::string& bytes){
  try{
    if(bytes.empty()||bytes.size()>8192)return {};const auto value=JsonParser::parse(bytes);
    const auto timeout=value.get("timeoutMs").number(-1);const auto nonce=value.get("nonce").string();
    if(!fields(value,{"type","protocol","nonce","timeoutMs"})||value.get("type").string()!="signin.diagnostic.challenge"||
        value.get("protocol").string()!=protocol||!nonceText(nonce)||!std::isfinite(timeout)||std::floor(timeout)!=timeout||timeout<1||timeout>30000)return {};
    return Challenge{nonce,uint32_t(timeout)};
  }catch(...){return {};}
}
std::optional<Code> response(const std::string& bytes,const std::string& nonce){
  try{
    if(bytes.empty()||bytes.size()>8192||!nonceText(nonce))return {};const auto value=JsonParser::parse(bytes);
    if(!fields(value,{"type","protocol","nonce","ok","code"})||value.get("type").string()!="signin.diagnostic.result"||
        value.get("protocol").string()!=protocol||value.get("nonce").string()!=nonce||!std::holds_alternative<bool>(value.get("ok").value))return {};
    const auto code=value.get("code").string();const bool ok=value.get("ok").boolean();
    if(ok)return code=="prepared"?std::optional(Code::prepared):std::nullopt;
    if(code=="refused")return Code::refused;if(code=="busy")return Code::busy;if(code=="expired")return Code::expired;
    if(code=="cancelled")return Code::cancelled;if(code=="failed")return Code::failed;if(code=="restore_pending")return Code::restorePending;
    if(code=="unsupported")return Code::unsupported;return {};
  }catch(...){return {};}
}
Result converse(HANDLE pipe,HANDLE stop,Process service,uint32_t width,uint32_t height,
    uint64_t begun,uint64_t deadline,const std::function<bool()>& current){
  Result result;
  try{
  const auto owns=[&]{try{return current&&current()&&!cancelled(stop)&&!expired(begun,deadline)&&sameServer(pipe,service);}catch(...){return false;}};
  if(!owns())return result;
  const auto first=readFrame(pipe,stop,begun,deadline,2000);if(!first){result.code=cancelled(stop)?Code::cancelled:(expired(begun,deadline)?Code::expired:Code::unsupported);return result;}
  const auto offer=challenge(*first);if(!offer){result.code=Code::unsupported;return result;}
  deadline=std::min(deadline,GetTickCount64()+offer->timeout);
  if(!owns())return result;
  const auto packet="{\"type\":\"signin.diagnostic.prepare\",\"protocol\":"+jsonQuote(protocol)+",\"purpose\":\"preparation-only\",\"nonce\":"+
    jsonQuote(offer->nonce)+",\"confirmed\":true,\"width\":"+std::to_string(width)+",\"height\":"+std::to_string(height)+'}';
  result.confirmationAttempted=true;result.code=Code::unverified;
  if(!writeFrame(pipe,stop,packet,begun,deadline))return result;
  // The real handler may prepare/clean up. Do not use the challenge's2s read
  // timeout here, and never reconnect/replay an ambiguously observed mutation.
  const auto final=readFrame(pipe,stop,begun,deadline,observationMs);
  if(!final||!owns())return result;const auto parsed=response(*final,offer->nonce);if(!parsed)return result;
  result.code=*parsed;result.observedResult=true;return result;
  }catch(...){result.code=result.confirmationAttempted?Code::unverified:Code::failed;return result;}
}
bool plainFile(HANDLE file,const std::filesystem::path& expected,uint64_t maximum,uint64_t* bytes=nullptr){
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
  if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&info)||info.nNumberOfLinks!=1||
      (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(file,&size)||
      size.QuadPart<=0||uint64_t(size.QuadPart)>maximum)return false;
  std::wstring path(32768,L'\0');const DWORD length=GetFinalPathNameByHandleW(file,path.data(),DWORD(path.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  if(!length||length>=path.size())return false;path.resize(length);
  if(_wcsicmp(path.c_str(),(L"\\\\?\\"+expected.wstring()).c_str()))return false;
  if(bytes)*bytes=uint64_t(size.QuadPart);return true;
}
WinHandle readLock(const std::filesystem::path& path,uint64_t maximum,uint64_t* bytes=nullptr){
  if(!trustedMachineFile(path)||!trustedMachineFile(path.parent_path()))return {};
  WinHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(file.value==INVALID_HANDLE_VALUE||!plainFile(file.value,path,maximum,bytes)||!trustedMachineFile(path))return {};
  return file;
}
std::optional<ToolPin> selfPin(const std::filesystem::path& bin){
  uint64_t length=0;auto file=readLock(bin/L"signin-diagnostic-tool.json",4096,&length);if(!file.value)return {};
  std::string bytes(size_t(length),'\0');DWORD read=0;
  if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())return {};
  char extra=0;if(!ReadFile(file.value,&extra,1,&read,nullptr)||read)return {};
  return parseToolPinMetadata(bytes,bin/L"MultitapHostTool.exe",MULTITAP_HOST_VERSION_UTF8);
}
bool matchesHash(HANDLE file,const ToolPin& pin){
  LARGE_INTEGER zero{};if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return false;
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<uint8_t> object;
  struct Cleanup{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}}cleanup{algorithm,hash};
  DWORD length=0,got=0;
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
      BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&got,0)<0)return false;
  object.resize(length);if(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)<0)return false;
  std::array<uint8_t,65536> buffer{};uint64_t total=0;
  for(;;){DWORD read=0;if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&read,nullptr))return false;if(!read)break;
    total+=read;if(total>pin.bytes||BCryptHashData(hash,buffer.data(),read,0)<0)return false;}
  Nonce value{};return total==pin.bytes&&BCryptFinishHash(hash,value.data(),DWORD(value.size()),0)>=0&&value==pin.sha256;
}
bool fileVersion(const std::filesystem::path& path,const std::string& expected){
  DWORD unused=0;const DWORD size=GetFileVersionInfoSizeW(path.c_str(),&unused);if(!size||size>1048576)return false;
  std::vector<uint8_t> bytes(size);VS_FIXEDFILEINFO* value=nullptr;UINT length=0;
  if(!GetFileVersionInfoW(path.c_str(),0,size,bytes.data())||!VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&value),&length)||
      length<sizeof(*value)||value->dwSignature!=0xfeef04bd)return false;
  return std::to_string(HIWORD(value->dwFileVersionMS))+'.'+std::to_string(LOWORD(value->dwFileVersionMS))+'.'+
    std::to_string(HIWORD(value->dwFileVersionLS))+'.'+std::to_string(LOWORD(value->dwFileVersionLS))==expected;
}
std::vector<uint8_t> selfAdminSid(DWORD session){
  HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))return {};WinHandle token(raw);
  TOKEN_ELEVATION elevation{};DWORD got=0,tokenSession=UINT32_MAX,container=1;
  if(!GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&got)||!elevation.TokenIsElevated||
      !GetTokenInformation(token.value,TokenSessionId,&tokenSession,sizeof(tokenSession),&got)||tokenSession!=session||
      !GetTokenInformation(token.value,TokenIsAppContainer,&container,sizeof(container),&got)||container)return {};
  DWORD bytes=0;GetTokenInformation(token.value,TokenGroups,nullptr,0,&bytes);if(!bytes||bytes>65536)return {};
  std::vector<uint8_t> groups(bytes);if(!GetTokenInformation(token.value,TokenGroups,groups.data(),bytes,&bytes))return {};
  const auto* list=reinterpret_cast<TOKEN_GROUPS*>(groups.data());bool admin=false;
  for(DWORD index=0;index<list->GroupCount;++index){const auto& group=list->Groups[index];
    if(IsWellKnownSid(group.Sid,WinBuiltinAdministratorsSid)&&(group.Attributes&SE_GROUP_ENABLED)&&!(group.Attributes&SE_GROUP_USE_FOR_DENY_ONLY))admin=true;}
  if(!admin)return {};
  bytes=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);if(!bytes||bytes>16384)return {};
  std::vector<uint8_t> user(bytes);if(!GetTokenInformation(token.value,TokenUser,user.data(),bytes,&bytes))return {};
  const auto sid=reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;if(!IsValidSid(sid))return {};
  std::vector<uint8_t> result(GetLengthSid(sid));if(!CopySid(DWORD(result.size()),result.data(),sid))return {};return result;
}
struct Peer {
  SC_HANDLE manager=nullptr,service=nullptr;WinHandle process,serviceImage,toolImage;
  Process serviceIdentity{},selfIdentity{};DWORD console=0;std::vector<uint8_t> sid;std::wstring image;
  ~Peer(){if(service)CloseServiceHandle(service);if(manager)CloseServiceHandle(manager);}
  bool current()const{
    try{
      if(identity(GetCurrentProcess())!=selfIdentity||console!=WTSGetActiveConsoleSessionId()||selfAdminSid(console)!=sid||
          identity(process.value)!=serviceIdentity||WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT)return false;
      SERVICE_STATUS_PROCESS status{};DWORD got=0;
      return QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&got)&&
        status.dwCurrentState==SERVICE_RUNNING&&status.dwProcessId==serviceIdentity.pid&&
        brokerServiceExecutable(service)==image&&brokerProcessExecutable(process.value,image);
    }catch(...){return false;}
  }
};
std::shared_ptr<Peer> authenticate(){
  try{
    std::wstring selfPath(32768,L'\0');const DWORD selfLength=GetModuleFileNameW(nullptr,selfPath.data(),DWORD(selfPath.size()));
    if(!selfLength||selfLength>=selfPath.size())return {};selfPath.resize(selfLength);
    if(_wcsicmp(std::filesystem::path(selfPath).filename().c_str(),L"MultitapHostTool.exe"))return {};
    auto peer=std::make_shared<Peer>();peer->selfIdentity=identity(GetCurrentProcess());
    if(!valid(peer->selfIdentity)||!ProcessIdToSessionId(GetCurrentProcessId(),&peer->console)||!peer->console||
        peer->console==UINT32_MAX||peer->console!=WTSGetActiveConsoleSessionId())return {};
    peer->sid=selfAdminSid(peer->console);if(peer->sid.empty())return {};
    peer->manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!peer->manager)return {};
    peer->service=OpenServiceW(peer->manager,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);if(!peer->service)return {};
    SERVICE_STATUS_PROCESS status{};DWORD got=0;
    if(!QueryServiceStatusEx(peer->service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&got)||
        status.dwCurrentState!=SERVICE_RUNNING||!status.dwProcessId||status.dwProcessId==GetCurrentProcessId())return {};
    peer->image=brokerServiceExecutable(peer->service);const auto bin=std::filesystem::path(peer->image).parent_path();
    const auto pin=selfPin(bin);if(!pin||!brokerProcessExecutable(GetCurrentProcess(),pin->path.wstring()))return {};
    uint64_t actualBytes=0;peer->toolImage=readLock(pin->path,128*1024*1024,&actualBytes);
    peer->serviceImage=readLock(peer->image,128*1024*1024);
    if(!peer->toolImage.value||!peer->serviceImage.value||actualBytes!=pin->bytes||!matchesHash(peer->toolImage.value,*pin)||
        !fileVersion(pin->path,pin->buildTag)||!fileVersion(peer->image,pin->buildTag))return {};
    peer->process=WinHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,status.dwProcessId));
    if(!peer->process.value||!brokerProcessExecutable(peer->process.value,peer->image))return {};
    peer->serviceIdentity=identity(peer->process.value);if(!valid(peer->serviceIdentity)||!peer->current())return {};return peer;
  }catch(...){return {};}
}
}
std::optional<Request> parseCommand(int argc,const wchar_t* const* argv){
  if(argc!=5||!argv||!argv[0]||!argv[1]||!argv[2]||!argv[3]||!argv[4]||
      _wcsicmp(std::filesystem::path(argv[0]).filename().c_str(),L"MultitapHostTool.exe")||
      std::wstring(argv[1])!=L"--signin-diagnostic-prepare-v1"||std::wstring(argv[2])!=L"--confirmed"||
      std::wstring(argv[3])!=L"1280"||std::wstring(argv[4])!=L"800")return {};
  return Request{1280,800};
}
Result requestPreparation(uint32_t width,uint32_t height,HANDLE stopEvent){
  Result result;
  try{
    if(width<320||width>8192||height<180||height>8192||!ownedOutputSize(width,height))return result;
    const auto begun=GetTickCount64(),deadline=begun+observationMs;WinHandle ownedStop;
    if(!stopEvent){ownedStop=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));stopEvent=ownedStop.value;}
    if(cancelled(stopEvent)){result.code=Code::cancelled;return result;}
    const auto peer=authenticate();if(!peer)return result;
    const auto name=endpoint(peer->serviceIdentity);if(name.empty())return result;WinHandle pipe;
    const auto connectUntil=std::min(deadline,GetTickCount64()+3000);bool sawBusy=false;
    for(;;){
      if(cancelled(stopEvent)||expired(begun,connectUntil)||!peer->current()){result.code=cancelled(stopEvent)?Code::cancelled:(sawBusy?Code::busy:Code::unsupported);return result;}
      pipe=WinHandle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
      if(pipe.value!=INVALID_HANDLE_VALUE)break;
      const auto code=GetLastError();if(code!=ERROR_PIPE_BUSY){result.code=Code::unsupported;return result;}
      sawBusy=true;
      if(WaitForSingleObject(stopEvent,50)!=WAIT_TIMEOUT){result.code=Code::cancelled;return result;}
    }
    return converse(pipe.value,stopEvent,peer->serviceIdentity,width,height,begun,deadline,[peer]{return peer->current();});
  }catch(...){return result;}
}
}
