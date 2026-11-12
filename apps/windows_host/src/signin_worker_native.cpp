#include "service_worker_drain.hpp"
#include "signin_worker_native.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace host::signin::native {
namespace {
Status ready(){return {Outcome::ready,Step::none,0};}
[[noreturn]] void fail(Step step,DWORD error=0){throw NativeFailure{{Outcome::refused,step,error}};}
struct ScHandle {SC_HANDLE value=nullptr;~ScHandle(){if(value)CloseServiceHandle(value);}};
struct WtsBuffer {LPWSTR value=nullptr;~WtsBuffer(){if(value)WTSFreeMemory(value);}};
std::vector<uint8_t> tokenInfo(HANDLE token,TOKEN_INFORMATION_CLASS kind){
  DWORD size=0;GetTokenInformation(token,kind,nullptr,0,&size);
  if(!size||size>65536)fail(Step::ownToken,GetLastError());
  std::vector<uint8_t> bytes(size);
  if(!GetTokenInformation(token,kind,bytes.data(),size,&size))fail(Step::ownToken,GetLastError());
  return bytes;
}
template<class T>T tokenValue(HANDLE token,TOKEN_INFORMATION_CLASS kind){
  T value{};DWORD size=0;
  if(!GetTokenInformation(token,kind,&value,sizeof(value),&size)||size!=sizeof(value))fail(Step::ownToken,GetLastError());
  return value;
}
bool tokenIsSystem(HANDLE token){
  const auto bytes=tokenInfo(token,TokenUser);
  const auto user=reinterpret_cast<const TOKEN_USER*>(bytes.data());
  return IsValidSid(user->User.Sid)&&IsWellKnownSid(user->User.Sid,WinLocalSystemSid);
}
std::string tokenSid(HANDLE token){
  const auto bytes=tokenInfo(token,TokenUser);
  const auto user=reinterpret_cast<const TOKEN_USER*>(bytes.data());LPWSTR value=nullptr;
  if(!IsValidSid(user->User.Sid)||!ConvertSidToStringSidW(user->User.Sid,&value))fail(Step::account,GetLastError());
  std::string out;for(const wchar_t* p=value;*p;++p){if(*p>127){LocalFree(value);fail(Step::account);}out.push_back(char(*p));}
  LocalFree(value);return out;
}
bool enabledPrivilege(HANDLE token,const wchar_t* name){
  LUID wanted{};if(!LookupPrivilegeValueW(nullptr,name,&wanted))return false;
  const auto bytes=tokenInfo(token,TokenPrivileges);
  const auto values=reinterpret_cast<const TOKEN_PRIVILEGES*>(bytes.data());
  if(values->PrivilegeCount>(bytes.size()-offsetof(TOKEN_PRIVILEGES,Privileges))/sizeof(LUID_AND_ATTRIBUTES))return false;
  for(DWORD i=0;i<values->PrivilegeCount;++i){const auto& value=values->Privileges[i];
    if(value.Luid.LowPart==wanted.LowPart&&value.Luid.HighPart==wanted.HighPart)return (value.Attributes&SE_PRIVILEGE_ENABLED)!=0;
  }return false;
}
std::wstring imagePath(HANDLE process){
  std::wstring path(32768,L'\0');DWORD count=DWORD(path.size());
  if(!QueryFullProcessImageNameW(process,0,path.data(),&count)||!count||count>=path.size())fail(Step::serviceImage,GetLastError());
  path.resize(count);return path;
}
bool sameText(const std::wstring& a,const std::wstring& b){return CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_EQUAL;}
bool objectName(HANDLE object,const wchar_t* expected){
  std::array<wchar_t,64> name{};DWORD needed=0;
  return object&&GetUserObjectInformationW(object,UOI_NAME,name.data(),DWORD(sizeof(name)),&needed)&&needed<=sizeof(name)&&_wcsicmp(name.data(),expected)==0;
}
bool serviceMatches(const Process& expected,HANDLE held,const std::wstring& expectedImage,
    detail::ServiceIdentityPurpose purpose=detail::ServiceIdentityPurpose::ordinary){
  if(!valid(expected)||processIdentity(held)!=expected||WaitForSingleObject(held,0)!=WAIT_TIMEOUT)return false;
  ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};
  if(!manager.value)return false;
  ScHandle service{OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG)};
  if(!service.value)return false;
  SERVICE_STATUS_PROCESS status{};DWORD count=0;
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&count)||
      !detail::serviceIdentityPhaseAllowed(status.dwCurrentState,purpose)||status.dwProcessId!=expected.pid)return false;
  if(purpose==detail::ServiceIdentityPurpose::sealedOriginalRestoration){
    DWORD session=UINT32_MAX;HANDLE raw=nullptr;
    if(!ProcessIdToSessionId(expected.pid,&session)||session!=0||!OpenProcessToken(held,TOKEN_QUERY,&raw))return false;
    Handle token(raw);
    if(!tokenIsSystem(token.get())||tokenValue<TOKEN_TYPE>(token.get(),TokenType)!=TokenPrimary||
        tokenValue<DWORD>(token.get(),TokenSessionId)!=0||tokenValue<DWORD>(token.get(),TokenIsAppContainer)||
        tokenValue<DWORD>(token.get(),TokenIsRestricted))return false;
  }
  QueryServiceConfigW(service.value,nullptr,0,&count);if(!count||count>65536)return false;
  std::vector<uint8_t> bytes(count);
  if(!QueryServiceConfigW(service.value,reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data()),count,&count))return false;
  const auto config=reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(bytes.data());
  if(!config->lpServiceStartName||(_wcsicmp(config->lpServiceStartName,L"LocalSystem")!=0&&_wcsicmp(config->lpServiceStartName,L"NT AUTHORITY\\SYSTEM")!=0))return false;
  if(!config->lpBinaryPathName||!sameText(config->lpBinaryPathName,L"\""+expectedImage+L"\" --service"))return false;
  return sameText(imagePath(held),expectedImage);
}
Handle pinnedOwnImage(const std::wstring& path,const std::array<uint8_t,32>& expected){
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
  if(!file||!GetFileInformationByHandle(file.get(),&info)||!GetFileSizeEx(file.get(),&length)||
      GetFileType(file.get())!=FILE_TYPE_DISK||info.nNumberOfLinks!=1||
      (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||length.QuadPart<=0||length.QuadPart>128*1024*1024||!nonzero(expected))fail(Step::serviceImage,GetLastError());
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  struct Close {BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Close(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}}close{algorithm,hash};
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)fail(Step::serviceImage);
  DWORD objectBytes=0,returned=0;
  if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&returned,0)<0||objectBytes>65536)fail(Step::serviceImage);
  std::vector<uint8_t> object(objectBytes);std::array<uint8_t,32> digest{};
  if(BCryptCreateHash(algorithm,&hash,object.data(),objectBytes,nullptr,0,0)<0)fail(Step::serviceImage);
  std::array<uint8_t,65536> buffer{};DWORD read=0;int64_t total=0;
  do{if(!ReadFile(file.get(),buffer.data(),DWORD(buffer.size()),&read,nullptr))fail(Step::serviceImage,GetLastError());total+=read;
    if(total>length.QuadPart||BCryptHashData(hash,buffer.data(),read,0)<0)fail(Step::serviceImage);
  }while(read);
  if(total!=length.QuadPart||BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0)<0||digest!=expected)fail(Step::serviceImage);
  return file;
}
std::wstring nonceText(const Nonce& nonce){constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring value;value.reserve(64);for(const auto c:nonce){value.push_back(hex[c>>4]);value.push_back(hex[c&15]);}return value;}
bool contextValid(const Context& context){
  return valid(context.service)&&context.serviceIsLocalSystem&&context.leaseOwned&&valid(context.lease)&&
    nonzero(context.stream)&&nonzero(context.baseline)&&nonzero(context.nonce)&&
    (context.unknownAccountAllowed?context.configuredSid.empty():accountSid(context.configuredSid))&&context.consoleSession&&context.consoleSession!=UINT32_MAX&&
    context.workerGeneration&&context.mediaEpoch&&context.ownedOutputMask==1&&
    context.assigned&&context.clientOptIn&&context.clientTransitions&&context.signinRuntimeQualified;
}
}
Process processIdentity(HANDLE process){
  FILETIME created{},exited{},kernel{},user{};const DWORD pid=GetProcessId(process);
  if(!pid||!GetProcessTimes(process,&created,&exited,&kernel,&user))fail(Step::serviceLifetime,GetLastError());
  return {pid,(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime};
}
static Status launchMaterial(Process expected,uint32_t consoleSession,
    const Nonce& imageSha,OwnSystemLaunchMaterial& material,detail::ServiceIdentityPurpose purpose){
  try{
    if(!valid(expected)||!consoleSession||consoleSession==UINT32_MAX||!nonzero(imageSha)||
        !material.image.empty()||material.imageLock||material.token||expected!=processIdentity(GetCurrentProcess()))fail(Step::admission);
    DWORD ownSession=UINT32_MAX;
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)||ownSession!=0)fail(Step::session,GetLastError());
    auto image=imagePath(GetCurrentProcess());
    if(!serviceMatches(expected,GetCurrentProcess(),image,purpose))fail(Step::service);
    auto imageLock=pinnedOwnImage(image,imageSha);
    HANDLE raw=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&raw))fail(Step::ownToken,GetLastError());
    Handle serviceToken(raw);
    if(!tokenIsSystem(serviceToken.get())||tokenValue<TOKEN_TYPE>(serviceToken.get(),TokenType)!=TokenPrimary||
        tokenValue<DWORD>(serviceToken.get(),TokenSessionId)!=0||tokenValue<DWORD>(serviceToken.get(),TokenIsAppContainer)||
        tokenValue<DWORD>(serviceToken.get(),TokenIsRestricted))fail(Step::ownToken);
    if(!enabledPrivilege(serviceToken.get(),SE_TCB_NAME))fail(Step::privilege,ERROR_PRIVILEGE_NOT_HELD);
    HANDLE primary=nullptr;
    if(!DuplicateTokenEx(serviceToken.get(),TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY|TOKEN_ADJUST_DEFAULT|TOKEN_ADJUST_SESSIONID,
        nullptr,SecurityImpersonation,TokenPrimary,&primary))fail(Step::createToken,GetLastError());
    Handle token(primary);
    DWORD session=consoleSession;
    if(!SetTokenInformation(token.get(),TokenSessionId,&session,sizeof(session))||
        tokenValue<DWORD>(token.get(),TokenSessionId)!=session||!tokenIsSystem(token.get()))fail(Step::createToken,GetLastError());
    material.image=std::move(image);material.imageLock=std::move(imageLock);material.token=std::move(token);
    return ready();
  }catch(const NativeFailure& failure){return failure.status;}
  catch(...){return {Outcome::failed,Step::createToken,0};}
}
Status ownSystemLaunchMaterial(Process expected,uint32_t consoleSession,const Nonce& imageSha,OwnSystemLaunchMaterial& material){
  return launchMaterial(expected,consoleSession,imageSha,material,detail::ServiceIdentityPurpose::ordinary);
}
Status ownSystemRestorationLaunchMaterial(Process expected,uint32_t consoleSession,const Nonce& imageSha,OwnSystemLaunchMaterial& material){
  return launchMaterial(expected,consoleSession,imageSha,material,detail::ServiceIdentityPurpose::sealedOriginalRestoration);
}
static Status parentPeer(Process expected,HANDLE held,HANDLE pipe,const Nonce& imageSha,Handle& imageLock,
    detail::ServiceIdentityPurpose purpose){
  try{
    DWORD flags=0;ULONG server=0;
    if(imageLock||!GetHandleInformation(held,&flags)||(flags&HANDLE_FLAG_INHERIT)||
        !GetNamedPipeServerProcessId(pipe,&server)||server!=expected.pid||
        !serviceMatches(expected,held,imagePath(GetCurrentProcess()),purpose))fail(Step::binding);
    imageLock=pinnedOwnImage(imagePath(GetCurrentProcess()),imageSha);
    return ready();
  }catch(const NativeFailure& failure){return failure.status;}
  catch(...){return {Outcome::failed,Step::binding,0};}
}
Status verifyInstalledParentPeer(Process expected,HANDLE held,HANDLE pipe,const Nonce& imageSha,Handle& imageLock){
  return parentPeer(expected,held,pipe,imageSha,imageLock,detail::ServiceIdentityPurpose::ordinary);
}
Status verifyRestorationParentPeer(Process expected,HANDLE held,HANDLE pipe,const Nonce& imageSha,Handle& imageLock){
  return parentPeer(expected,held,pipe,imageSha,imageLock,detail::ServiceIdentityPurpose::sealedOriginalRestoration);
}
static Status parentCurrent(Process expected,HANDLE held,HANDLE pipe,detail::ServiceIdentityPurpose purpose){
  try{
    DWORD flags=0;ULONG server=0;
    if(!GetHandleInformation(held,&flags)||(flags&HANDLE_FLAG_INHERIT)||
        !GetNamedPipeServerProcessId(pipe,&server)||server!=expected.pid||
        !serviceMatches(expected,held,imagePath(GetCurrentProcess()),purpose))fail(Step::binding);
    return ready();
  }catch(const NativeFailure& failure){return failure.status;}
  catch(...){return {Outcome::failed,Step::binding,0};}
}
Status verifyInstalledParentCurrent(Process expected,HANDLE held,HANDLE pipe){
  return parentCurrent(expected,held,pipe,detail::ServiceIdentityPurpose::ordinary);
}
Status verifyRestorationParentCurrent(Process expected,HANDLE held,HANDLE pipe){
  return parentCurrent(expected,held,pipe,detail::ServiceIdentityPurpose::sealedOriginalRestoration);
}
ConsoleFacts observeConsole(uint32_t session){
  ConsoleFacts result;result.session=session;result.observedAt=GetTickCount64();
  if(!session||session==UINT32_MAX||WTSGetActiveConsoleSessionId()!=session)return result;
  WtsBuffer protocol;DWORD bytes=0;
  if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSClientProtocolType,&protocol.value,&bytes)||bytes!=sizeof(USHORT))return result;
  result.consoleProtocol=*reinterpret_cast<const USHORT*>(protocol.value)==0;if(!result.consoleProtocol)return result;
  WtsBuffer information;
  if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSSessionInfoEx,&information.value,&bytes)||bytes<sizeof(WTSINFOEXW))return result;
  const auto info=reinterpret_cast<const WTSINFOEXW*>(information.value);
  if(info->Level!=1||info->Data.WTSInfoExLevel1.SessionId!=session)return result;
  const auto& view=info->Data.WTSInfoExLevel1;
  if(view.SessionState!=WTSActive&&view.SessionState!=WTSConnected)return result;
  HANDLE raw=nullptr;SetLastError(ERROR_SUCCESS);const BOOL queried=WTSQueryUserToken(session,&raw);const DWORD error=GetLastError();Handle user(raw);
  if(!queried){
    if(error==ERROR_NO_TOKEN&&view.UserName[0]==L'\0'){result.windows=WindowsState::noUser;result.valid=true;}
    return result;
  }
  if(tokenValue<DWORD>(user.get(),TokenSessionId)!=session||tokenIsSystem(user.get()))return result;
  result.signedInSid=tokenSid(user.get());
  if(view.SessionFlags==WTS_SESSIONSTATE_LOCK)result.windows=WindowsState::locked;
  else if(view.SessionFlags==WTS_SESSIONSTATE_UNLOCK)result.windows=WindowsState::unlocked;
  result.valid=result.windows!=WindowsState::unknown;return result;
}
WorkerFacts observeWorker(){
  WorkerFacts result;result.process=processIdentity(GetCurrentProcess());
  HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))fail(Step::ownToken,GetLastError());Handle token(raw);
  result.localSystem=tokenIsSystem(token.get())&&tokenValue<TOKEN_TYPE>(token.get(),TokenType)==TokenPrimary&&
    !tokenValue<DWORD>(token.get(),TokenIsAppContainer)&&!tokenValue<DWORD>(token.get(),TokenIsRestricted);
  DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session))fail(Step::session,GetLastError());
  result.session=session;
  if(tokenValue<DWORD>(token.get(),TokenSessionId)!=session)result.localSystem=false;
  result.stationWinSta0=objectName(GetProcessWindowStation(),L"WinSta0");
  result.threadWinlogon=objectName(GetThreadDesktop(GetCurrentThreadId()),L"Winlogon");
  result.threadDefault=objectName(GetThreadDesktop(GetCurrentThreadId()),L"Default");
  HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
  result.inputWinlogon=objectName(input,L"Winlogon");result.inputDefault=objectName(input,L"Default");if(input)CloseDesktop(input);
  return result;
}
Status checkConsole(const Context& context,const ConsoleFacts& facts,uint64_t now){
  if(!contextValid(context))return {Outcome::refused,Step::admission,0};
  if(!facts.valid||!facts.consoleProtocol||facts.session!=context.consoleSession||!facts.observedAt||facts.observedAt>now||now-facts.observedAt>1000)return {Outcome::refused,Step::session,0};
  if(facts.windows==WindowsState::noUser){if(!facts.signedInSid.empty())return {Outcome::refused,Step::account,0};}
  else if(facts.windows==WindowsState::locked){if(context.unknownAccountAllowed||!accountSid(context.configuredSid)||facts.signedInSid!=context.configuredSid)return {Outcome::refused,Step::account,0};}
  else return {Outcome::handoffRequired,Step::session,0};
  return ready();
}
bool validBinding(const Binding& binding,const Context& context){
  return contextValid(context)&&valid(binding.worker)&&binding.worker!=context.service&&
    binding.stream==context.stream&&binding.service==context.service&&binding.lease==context.lease&&
    binding.baseline==context.baseline&&binding.nonce==context.nonce&&binding.consoleSession==context.consoleSession&&
    binding.generation==context.workerGeneration&&binding.mediaEpoch==context.mediaEpoch&&binding.role==Role::signin;
}
Status checkWorker(const Binding& binding,const Context& context,const ConsoleFacts& console,const WorkerFacts& worker,uint64_t now){
  if(!validBinding(binding,context)||worker.process!=binding.worker)return {Outcome::refused,Step::binding,0};
  const auto eligible=checkConsole(context,console,now);if(!eligible)return eligible;
  if(!worker.localSystem||worker.session!=context.consoleSession)return {Outcome::refused,Step::ownToken,0};
  if(!worker.stationWinSta0||!worker.threadWinlogon||!worker.inputWinlogon)return {Outcome::handoffRequired,Step::desktop,0};
  return ready();
}
OwnedWorker::~OwnedWorker(){(void)stopAndWait(0);}
bool OwnedWorker::connect(uint32_t milliseconds){
  if(!pipe_||!process()||milliseconds>5000)return false;
  const auto ownedPeer=[&]{
    try{ULONG peer=0;return GetNamedPipeClientProcessId(pipe_.get(),&peer)&&peer==identity_.pid&&
      processIdentity(process())==identity_&&WaitForSingleObject(process(),0)==WAIT_TIMEOUT;}
    catch(...){return false;}
  };
  Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
  OVERLAPPED operation{};operation.hEvent=event.get();
  if(ConnectNamedPipe(pipe_.get(),&operation))return ownedPeer();
  const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return ownedPeer();if(error!=ERROR_IO_PENDING)return false;
  const HANDLE handles[]{event.get(),process()};const auto wait=WaitForMultipleObjects(2,handles,FALSE,milliseconds);
  if(wait!=WAIT_OBJECT_0){CancelIoEx(pipe_.get(),&operation);DWORD ignored=0;GetOverlappedResult(pipe_.get(),&operation,&ignored,TRUE);return false;}
  DWORD ignored=0;return GetOverlappedResult(pipe_.get(),&operation,&ignored,FALSE)!=FALSE&&ownedPeer();
}
bool OwnedWorker::stopAndWait(uint32_t milliseconds){
  if(milliseconds>5000)return false;pipe_.reset();
  // EOF first permits orderly capture/encoder teardown. Only after the bounded
  // wait does closing this owned job request forced cleanup of this own child.
  if(!process()||WaitForSingleObject(process(),milliseconds)==WAIT_OBJECT_0){job_.reset();return true;}
  job_.reset();
  // This is only the HANDLE returned directly by this object's CreateProcess.
  // A failed job assignment must not strand an unassigned suspended SYSTEM
  // child. Never reopen a PID or terminate another process by name.
  if(process()&&!jobAssigned_&&WaitForSingleObject(process(),0)==WAIT_TIMEOUT)
    (void)TerminateProcess(process(),1);
  return WaitForSingleObject(process(),0)==WAIT_OBJECT_0;
}
Handle OwnedWorker::takeUnexited(){
  if(!process()||WaitForSingleObject(process(),0)!=WAIT_TIMEOUT)return {};
  HANDLE copy=nullptr;
  if(!DuplicateHandle(GetCurrentProcess(),process(),GetCurrentProcess(),&copy,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))fail(Step::childIdentity,GetLastError());
  return Handle(copy);
}
Status launchWorker(const LaunchRequest& request,OwnedWorker& owned){
  if(owned.process()||owned.pipe_||owned.job_)return {Outcome::refused,Step::childIdentity,0};
  try{
    auto physicalCreation=service_workers::reserveCurrent(service_workers::Role::userWorker,true);
    Handle physicalJob(physicalCreation.createTrackingJob());
    owned.retainedProcess_=physicalCreation.observer();
    const auto& context=request.context;
    if(!contextValid(context)||!request.scopeGeneration||context.service!=processIdentity(GetCurrentProcess()))fail(Step::admission);
    DWORD ownSession=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)||ownSession!=0)fail(Step::session,GetLastError());
    const auto image=imagePath(GetCurrentProcess());
    if(!serviceMatches(context.service,GetCurrentProcess(),image))fail(Step::service);
    auto imageLock=pinnedOwnImage(image,request.installedImageSha256);
    HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&raw))fail(Step::ownToken,GetLastError());Handle serviceToken(raw);
    if(!tokenIsSystem(serviceToken.get())||tokenValue<TOKEN_TYPE>(serviceToken.get(),TokenType)!=TokenPrimary||tokenValue<DWORD>(serviceToken.get(),TokenSessionId)!=0||tokenValue<DWORD>(serviceToken.get(),TokenIsAppContainer)||tokenValue<DWORD>(serviceToken.get(),TokenIsRestricted))fail(Step::ownToken);
    if(!enabledPrivilege(serviceToken.get(),SE_TCB_NAME))fail(Step::privilege,ERROR_PRIVILEGE_NOT_HELD);
    const auto initialConsole=observeConsole(context.consoleSession);
    const auto scope=checkConsole(context,initialConsole,GetTickCount64());if(!scope)return scope;
    HANDLE primary=nullptr;
    if(!DuplicateTokenEx(serviceToken.get(),TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY|TOKEN_ADJUST_DEFAULT|TOKEN_ADJUST_SESSIONID,nullptr,SecurityImpersonation,TokenPrimary,&primary))fail(Step::createToken,GetLastError());Handle token(primary);
    DWORD session=context.consoleSession;
    if(!SetTokenInformation(token.get(),TokenSessionId,&session,sizeof(session))||tokenValue<DWORD>(token.get(),TokenSessionId)!=session||!tokenIsSystem(token.get()))fail(Step::createToken,GetLastError());
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr))fail(Step::pipe,GetLastError());
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    owned.pipeName_=L"\\\\.\\pipe\\MultitapHost.Signin."+std::to_wstring(context.service.pid)+L"."+nonceText(context.nonce);
    owned.pipe_=Handle(CreateNamedPipeW(owned.pipeName_.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&security));const DWORD pipeError=GetLastError();LocalFree(descriptor);
    if(!owned.pipe_)fail(Step::pipe,pipeError);
    owned.job_=Handle(CreateJobObjectW(nullptr,nullptr));if(!owned.job_)fail(Step::isolateProcess,GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(owned.job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))fail(Step::isolateProcess,GetLastError());
    struct Environment {void* value=nullptr;~Environment(){if(value)DestroyEnvironmentBlock(value);}}environment;
    if(!CreateEnvironmentBlock(&environment.value,token.get(),FALSE))fail(Step::createProcess,GetLastError());
    const auto finalConsole=observeConsole(session);
    const auto lastScope=checkConsole(context,finalConsole,GetTickCount64());if(!lastScope)return lastScope;
    std::wstring command=L"\""+image+L"\" --signin-worker-v1 --pipe "+owned.pipeName_+L" --service-pid "+std::to_wstring(context.service.pid);
    service_workers::JobListStartup physicalStartup(physicalJob.get(),owned.job_.get());auto& startup=physicalStartup.startup.StartupInfo;wchar_t desktop[]=L"winsta0\\Winlogon";startup.lpDesktop=desktop;PROCESS_INFORMATION child{};
    const auto slash=image.find_last_of(L"\\/");if(slash==std::wstring::npos)fail(Step::serviceImage);
    physicalCreation.requireLaunchAllowed();
    if(!CreateProcessAsUserW(token.get(),image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,environment.value,image.substr(0,slash).c_str(),&startup,&child))fail(Step::createProcess,GetLastError());
    owned.process_=Handle(child.hProcess);Handle thread(child.hThread);owned.identity_.pid=child.dwProcessId;owned.jobAssigned_=true;
    HANDLE physicalOriginal=owned.process_.release();
    try{physicalCreation.retain(physicalOriginal);}catch(...){owned.process_.reset(physicalOriginal);const auto known=owned.retainedProcess_.identity();if(known.known)owned.identity_={known.pid,known.birth};throw;}
    owned.process_.reset(physicalOriginal);
    // Exact root and outer job are retained before any fallible identity work.
    owned.identity_=processIdentity(owned.process());
    if(!valid(owned.identity_)||owned.identity_.pid==context.service.pid||!sameText(imagePath(owned.process()),image)||WaitForSingleObject(owned.process(),0)!=WAIT_TIMEOUT)fail(Step::childIdentity);
    owned.binding_={context.stream,context.service,owned.identity_,context.lease,
      context.baseline,context.nonce,context.consoleSession,context.workerGeneration,
      context.mediaEpoch,Role::signin};
    HANDLE delegated=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),owned.process(),&delegated,
        PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))fail(Step::binding,GetLastError());
    owned.delegatedServiceQuery_=uint64_t(reinterpret_cast<uintptr_t>(delegated));
    if(ResumeThread(thread.get())==DWORD(-1))fail(Step::createProcess,GetLastError());
    return ready();
  }catch(const NativeFailure& failure){return failure.status;}
  catch(...){return {Outcome::failed,Step::createProcess,0};}
}
Status Permit::current()const{return checkCurrent(false);}
Status Permit::currentForRelease()const{return checkCurrent(true);}
Status Permit::checkCurrent(bool releaseOnly)const{
  try{
    if(!authority_||!(releaseOnly?authority_->currentForRelease(binding_,targets_):authority_->current(binding_,targets_)))return {Outcome::refused,Step::lease,0};
    if(!serviceMatches(binding_.service,service_,imagePath(GetCurrentProcess())))return {Outcome::handoffRequired,Step::serviceLifetime,0};
    ULONG server=0;if(!GetNamedPipeServerProcessId(pipe_,&server)||server!=binding_.service.pid)return {Outcome::refused,Step::pipe,GetLastError()};
    // Sequence the observations before reading "now"; C++ argument evaluation
    // order must not manufacture a future observation and false refusal.
    const auto console=observeConsole(context_.consoleSession);
    const auto worker=observeWorker();
    return checkWorker(binding_,context_,console,worker,GetTickCount64());
  }catch(const NativeFailure& failure){return failure.status;}
  catch(...){return {Outcome::failed,Step::binding,0};}
}
std::unique_ptr<Permit> authorizeWorker(const Binding& binding,const Context& context,HANDLE serviceQuery,HANDLE pipe,std::span<const OwnedTarget> targets,const LeaseAuthority& authority,Status& status){
  status={Outcome::refused,Step::binding,0};
  if(!validBinding(binding,context)||targets.size()!=1||targets[0].mask!=1)return {};
  DWORD flags=0;if(!GetHandleInformation(serviceQuery,&flags)||(flags&HANDLE_FLAG_INHERIT))return {};
  auto permit=std::unique_ptr<Permit>(new Permit);permit->binding_=binding;permit->context_=context;permit->service_=serviceQuery;permit->pipe_=pipe;permit->targets_.assign(targets.begin(),targets.end());permit->authority_=&authority;
  status=permit->current();if(!status)return {};return permit;
}
}
