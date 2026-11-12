#pragma once
#include "native_display_digest.hpp"
#include "native_display_topology.hpp"
#include <ntsecapi.h>
namespace host {
struct NativeForeignBinding {
  LUID sourceAdapter{},targetAdapter{};UINT32 source=0,target=0,connector=0;
  std::string sourcePath,targetPath,monitorPath;bool complete=false;
};
inline auto nativeForeignBindingKey(const NativeForeignBinding& value){return std::tuple{value.sourceAdapter.HighPart,value.sourceAdapter.LowPart,value.source,value.targetAdapter.HighPart,value.targetAdapter.LowPart,value.target};}
inline auto nativeForeignBindingValue(const NativeForeignBinding& value){return std::tuple{value.sourcePath,value.targetPath,value.monitorPath,value.connector,value.complete};}
struct NativeRecoveryScope {DWORD session=0;std::string logon;uint64_t tick=0;std::vector<NativeForeignBinding> foreign;};
inline bool nativeRecoveryHex(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline void validateNativeRecoveryScope(const NativeRecoveryScope& scope,const NativeTopologyBaseline& baseline){
  if(!scope.session||scope.session==0xffffffff||!nativeRecoveryHex(scope.logon)||!scope.tick||scope.tick>INT64_MAX||scope.foreign.size()>64)throw std::runtime_error("native_recovery_scope_invalid");
  size_t count=0;std::set<decltype(nativeForeignBindingKey(NativeForeignBinding{}))> keys;
  for(const auto& path:baseline.ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,baseline.targets)){
    ++count;const auto found=std::find_if(scope.foreign.begin(),scope.foreign.end(),[&](const auto& value){return nativeSameLuid(value.sourceAdapter,path.sourceInfo.adapterId)&&value.source==path.sourceInfo.id&&nativeSameLuid(value.targetAdapter,path.targetInfo.adapterId)&&value.target==path.targetInfo.id;});
    if(found==scope.foreign.end()||!keys.insert(nativeForeignBindingKey(*found)).second)throw std::runtime_error("native_recovery_scope_invalid");
    for(const auto* text:{&found->sourcePath,&found->targetPath,&found->monitorPath}){if(text->size()>512||std::any_of(text->begin(),text->end(),[](unsigned char c){return c<32||c==127;}))throw std::runtime_error("native_recovery_scope_invalid");(void)wide(*text);}
    if(found->complete&&(found->sourcePath.empty()||found->targetPath.empty()||found->monitorPath.empty()))throw std::runtime_error("native_recovery_scope_invalid");
  }
  if(count!=scope.foreign.size())throw std::runtime_error("native_recovery_scope_invalid");
}
inline bool nativeRecoveryScopeComplete(const NativeRecoveryScope& scope){return std::all_of(scope.foreign.begin(),scope.foreign.end(),[](const auto& item){return item.complete&&!item.sourcePath.empty()&&!item.targetPath.empty()&&!item.monitorPath.empty();});}
inline bool nativeRecoveryLogonMatches(const NativeRecoveryScope& previous,const NativeRecoveryScope& fresh){return previous.session&&previous.session!=0xffffffff&&previous.session==fresh.session&&nativeRecoveryHex(previous.logon)&&previous.logon==fresh.logon&&previous.tick&&previous.tick<=INT64_MAX&&fresh.tick<=INT64_MAX&&fresh.tick>=previous.tick;}
inline bool nativeRecoveryScopeMatches(const NativeRecoveryScope& previous,const NativeRecoveryScope& fresh){
  if(!nativeRecoveryLogonMatches(previous,fresh)||!nativeRecoveryScopeComplete(previous)||!nativeRecoveryScopeComplete(fresh)||previous.foreign.size()!=fresh.foreign.size())return false;
  std::map<decltype(nativeForeignBindingKey(NativeForeignBinding{})),decltype(nativeForeignBindingValue(NativeForeignBinding{}))> a,b;
  for(const auto& item:previous.foreign)if(!a.emplace(nativeForeignBindingKey(item),nativeForeignBindingValue(item)).second)return false;
  for(const auto& item:fresh.foreign)if(!b.emplace(nativeForeignBindingKey(item),nativeForeignBindingValue(item)).second)return false;return a==b;
}
inline NativeRecoveryScope nativeUnlinkedRecoveryScope(HANDLE token,DWORD session){
  TOKEN_STATISTICS stats{};DWORD size=0,tokenSession=0;if(!session||session==0xffffffff||!GetTokenInformation(token,TokenStatistics,&stats,sizeof(stats),&size)||!GetTokenInformation(token,TokenSessionId,&tokenSession,sizeof(tokenSession),&size)||tokenSession!=session)throw std::runtime_error("native_recovery_scope_unavailable");
  GetTokenInformation(token,TokenUser,nullptr,0,&size);if(!size||size>4096)throw std::runtime_error("native_recovery_scope_unavailable");std::vector<BYTE> user(size);if(!GetTokenInformation(token,TokenUser,user.data(),size,&size))throw std::runtime_error("native_recovery_scope_unavailable");
  PSECURITY_LOGON_SESSION_DATA data=nullptr;const auto status=LsaGetLogonSessionData(&stats.AuthenticationId,&data);struct Release{PSECURITY_LOGON_SESSION_DATA value;~Release(){if(value)LsaFreeReturnBuffer(value);}}release{data};
  if(status<0||!data||data->Size<offsetof(SECURITY_LOGON_SESSION_DATA,LogonTime)+sizeof(LARGE_INTEGER)||!data->Sid||!IsValidSid(data->Sid)||!EqualSid(data->Sid,reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid)||(data->Session&&data->Session!=session)||data->LogonTime.QuadPart<=0||(data->LogonType!=Interactive&&data->LogonType!=RemoteInteractive&&data->LogonType!=CachedInteractive&&data->LogonType!=CachedRemoteInteractive))throw std::runtime_error("native_recovery_scope_unavailable");
  LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(data->Sid,&sid))throw std::runtime_error("native_recovery_scope_unavailable");struct LocalRelease{LPWSTR value;~LocalRelease(){LocalFree(value);}}sidRelease{sid};
  std::string material="native-recovery-logon-v1|"+std::to_string(session)+'|'+std::to_string(stats.AuthenticationId.HighPart)+':'+std::to_string(stats.AuthenticationId.LowPart)+'|'+std::to_string(data->LogonTime.QuadPart)+'|'+utf8(sid);struct Wipe{std::string& value;~Wipe(){SecureZeroMemory(value.data(),value.size());}}wipe{material};
  return {session,nativeBaselineDigest(material),GetTickCount64(),{}};
}
inline NativeRecoveryScope nativeTokenRecoveryScope(HANDLE token,DWORD session){
  auto scope=nativeUnlinkedRecoveryScope(token,session);DWORD size=0;TOKEN_ELEVATION_TYPE elevation{};
  if(!GetTokenInformation(token,TokenElevationType,&elevation,sizeof(elevation),&size))throw std::runtime_error("native_recovery_scope_unavailable");
  if(elevation==TokenElevationTypeDefault)return scope;
  if(elevation!=TokenElevationTypeFull&&elevation!=TokenElevationTypeLimited)throw std::runtime_error("native_recovery_scope_unavailable");
  TOKEN_LINKED_TOKEN link{};if(!GetTokenInformation(token,TokenLinkedToken,&link,sizeof(link),&size)||!link.LinkedToken)throw std::runtime_error("native_recovery_scope_unavailable");WinHandle other(link.LinkedToken);
  const auto userSid=[](HANDLE handle){DWORD length=0;GetTokenInformation(handle,TokenUser,nullptr,0,&length);if(!length||length>4096)throw std::runtime_error("native_recovery_scope_unavailable");std::vector<BYTE> bytes(length);if(!GetTokenInformation(handle,TokenUser,bytes.data(),length,&length))throw std::runtime_error("native_recovery_scope_unavailable");return bytes;};
  const auto originalUser=userSid(token),otherUser=userSid(other.value);TOKEN_ELEVATION_TYPE otherElevation{};DWORD otherSession=0;
  if(!EqualSid(reinterpret_cast<const TOKEN_USER*>(originalUser.data())->User.Sid,reinterpret_cast<const TOKEN_USER*>(otherUser.data())->User.Sid)||!GetTokenInformation(other.value,TokenSessionId,&otherSession,sizeof(otherSession),&size)||otherSession!=session||!GetTokenInformation(other.value,TokenElevationType,&otherElevation,sizeof(otherElevation),&size)||(elevation==TokenElevationTypeFull?otherElevation!=TokenElevationTypeLimited:otherElevation!=TokenElevationTypeFull))throw std::runtime_error("native_recovery_scope_unavailable");
  TOKEN_LINKED_TOKEN back{};if(!GetTokenInformation(other.value,TokenLinkedToken,&back,sizeof(back),&size)||!back.LinkedToken)throw std::runtime_error("native_recovery_scope_unavailable");WinHandle original(back.LinkedToken);TOKEN_STATISTICS a{},b{};DWORD backSession=0;
  if(!GetTokenInformation(token,TokenStatistics,&a,sizeof(a),&size)||!GetTokenInformation(original.value,TokenStatistics,&b,sizeof(b),&size)||!nativeSameLuid(a.AuthenticationId,b.AuthenticationId)||!GetTokenInformation(original.value,TokenSessionId,&backSession,sizeof(backSession),&size)||backSession!=session)throw std::runtime_error("native_recovery_scope_unavailable");
  const auto otherScope=nativeUnlinkedRecoveryScope(other.value,session);const auto low=std::min(scope.logon,otherScope.logon),high=std::max(scope.logon,otherScope.logon);scope.logon=nativeBaselineDigest("native-recovery-linked-logon-v1|"+low+'|'+high);scope.tick=GetTickCount64();return scope;
}
inline NativeRecoveryScope nativeCurrentRecoveryScope(){DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session))throw std::runtime_error("native_recovery_scope_unavailable");HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw std::runtime_error("native_recovery_scope_unavailable");WinHandle token(raw);return nativeTokenRecoveryScope(token.value,session);}
inline NativeRecoveryScope nativeSessionRecoveryScope(DWORD session){HANDLE raw=nullptr;if(!WTSQueryUserToken(session,&raw))throw std::runtime_error("native_recovery_scope_unavailable");WinHandle token(raw);return nativeTokenRecoveryScope(token.value,session);}
template<size_t N>inline std::string nativeRecoveryDeviceText(const wchar_t(&text)[N]){const auto end=std::find(text,text+N,L'\0');if(end==text+N)throw std::runtime_error("native_recovery_identity_unavailable");return utf8(std::wstring(text,end));}
inline NativeForeignBinding nativeQueryForeignBinding(const DISPLAYCONFIG_PATH_INFO& path){
  NativeForeignBinding result;result.sourceAdapter=path.sourceInfo.adapterId;result.source=path.sourceInfo.id;result.targetAdapter=path.targetInfo.adapterId;result.target=path.targetInfo.id;
  DISPLAYCONFIG_ADAPTER_NAME source{},target{};DISPLAYCONFIG_TARGET_DEVICE_NAME monitor{};
  source.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME,sizeof(source),result.sourceAdapter,0};target.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME,sizeof(target),result.targetAdapter,0};monitor.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(monitor),result.targetAdapter,result.target};
  try{const auto a=DisplayConfigGetDeviceInfo(&source.header),b=DisplayConfigGetDeviceInfo(&target.header),c=DisplayConfigGetDeviceInfo(&monitor.header);if(a==ERROR_SUCCESS)result.sourcePath=nativeRecoveryDeviceText(source.adapterDevicePath);if(b==ERROR_SUCCESS)result.targetPath=nativeRecoveryDeviceText(target.adapterDevicePath);if(c==ERROR_SUCCESS){result.monitorPath=nativeRecoveryDeviceText(monitor.monitorDevicePath);result.connector=monitor.connectorInstance;}result.complete=a==ERROR_SUCCESS&&b==ERROR_SUCCESS&&c==ERROR_SUCCESS&&!result.sourcePath.empty()&&!result.targetPath.empty()&&!result.monitorPath.empty();}catch(...){result.sourcePath.clear();result.targetPath.clear();result.monitorPath.clear();result.complete=false;}return result;
}
inline NativeRecoveryScope captureNativeRecoveryScope(const NativeTopologyBaseline& baseline,std::function<NativeRecoveryScope()> session=nativeCurrentRecoveryScope,std::function<NativeForeignBinding(const DISPLAYCONFIG_PATH_INFO&)> binding=nativeQueryForeignBinding){
  auto scope=session();for(const auto& path:baseline.ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,baseline.targets))scope.foreign.push_back(binding(path));validateNativeRecoveryScope(scope,baseline);return scope;
}
}
