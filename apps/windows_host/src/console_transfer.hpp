#pragma once
#include "machine.hpp"

namespace host {
// Only the caller's active RDP login may move to the local console. Account
// identities stay private; clients cannot name an arbitrary source or user.
struct ConsoleTransferObservation {
  DWORD source=0,console=0xffffffff;
  bool administrator=false,native=false,installed=false,started=false,restart=false;
  bool sourceActive=false,sourceIdentity=false,consoleUserKnown=false,consoleOccupied=false,consoleSameUser=false;
  bool sourceDisconnected=false;
  int protocol=-1;
};
inline const char* consoleTransferReason(const ConsoleTransferObservation& s,bool allowDisconnected=false){
  if(!s.administrator)return "administrator_required";
  if(!s.native)return "native_provider_required";
  if(!s.source||s.source==0xffffffff)return "interactive_session_required";
  if(!s.installed||!s.started)return "virtual_device_not_ready";
  if(s.restart)return "restart_required";
  if(s.protocol==0&&s.source==s.console)return "already_console";
  const bool existingDisconnected=allowDisconnected&&s.sourceDisconnected&&(s.protocol==0||s.protocol==2);
  if(!(s.protocol==2&&s.sourceActive)&&!existingDisconnected)return "active_rdp_session_required";
  if(!s.sourceIdentity)return "session_identity_unavailable";
  if(!s.console||s.console==0xffffffff||s.console==s.source)return "console_unavailable";
  if(!s.consoleUserKnown)return "console_identity_unavailable";
  if(s.consoleOccupied&&!s.consoleSameUser)return "console_in_use";
  return "";
}
struct ConsoleTransferAccess {
  std::function<ConsoleTransferObservation()> observe;
  std::function<bool(DWORD,DWORD)> connect;
  std::function<void()> pause;
};
struct ConsoleTransferResult {bool requested=false,completed=false;};
inline ConsoleTransferResult transferOwnConsole(const ConsoleTransferAccess& access,DWORD expectedSource,DWORD expectedConsole,bool allowDisconnected=false){
  auto observation=access.observe();
  const auto verify=[&](const ConsoleTransferObservation& s){
    const auto reason=consoleTransferReason(s,allowDisconnected);if(*reason)throw std::runtime_error(std::string("console_transfer_")+reason);
    if(s.source!=expectedSource||s.console!=expectedConsole)throw std::runtime_error("console_transfer_session_changed");
  };
  verify(observation);
  // Recheck user, console occupancy and session binding immediately before the
  // one OS call. Never retry a transfer whose completion is uncertain.
  observation=access.observe();verify(observation);
  if(!access.connect(observation.source,observation.console))throw std::runtime_error("console_transfer_rejected");
  ConsoleTransferResult result{true,false};
  for(unsigned attempt=0;attempt<10;++attempt){
    try{const auto after=access.observe();
      if(after.source==expectedSource&&after.console==expectedSource&&after.protocol==0&&after.sourceIdentity&&after.sourceActive){result.completed=true;break;}
    }catch(...){break;}
    access.pause();
  }
  return result;
}
struct ConsoleTransferWtsBuffer {
  LPWSTR value=nullptr;DWORD size=0;
  ~ConsoleTransferWtsBuffer(){if(value)WTSFreeMemory(value);}
  void query(DWORD session,WTS_INFO_CLASS kind){
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,kind,&value,&size)||!value||!size||size>8192)
      throw std::runtime_error("console_transfer_identity_unavailable");
  }
};
inline std::wstring consoleTransferAccountText(DWORD session,WTS_INFO_CLASS kind){
  ConsoleTransferWtsBuffer buffer;buffer.query(session,kind);
  if(buffer.size%sizeof(wchar_t))throw std::runtime_error("console_transfer_identity_unavailable");
  const auto bound=buffer.size/sizeof(wchar_t),length=wcsnlen_s(buffer.value,bound);
  if(length>=bound||length>1024)throw std::runtime_error("console_transfer_identity_unavailable");return {buffer.value,length};
}
inline std::vector<BYTE> consoleTransferAccountSid(DWORD session,bool& occupied){
  const auto username=consoleTransferAccountText(session,WTSUserName);occupied=!username.empty();if(!occupied)return {};
  const auto domain=consoleTransferAccountText(session,WTSDomainName);
  const auto account=domain.empty()?username:domain+L"\\"+username;
  DWORD sidSize=0,domainSize=0;SID_NAME_USE use{};
  LookupAccountNameW(nullptr,account.c_str(),nullptr,&sidSize,nullptr,&domainSize,&use);
  if(!sidSize||sidSize>SECURITY_MAX_SID_SIZE||domainSize>1024)throw std::runtime_error("console_transfer_identity_unavailable");
  std::vector<BYTE> sid(sidSize);std::wstring ignoredDomain(std::max<DWORD>(1,domainSize),L'\0');
  if(!LookupAccountNameW(nullptr,account.c_str(),sid.data(),&sidSize,ignoredDomain.data(),&domainSize,&use)||use!=SidTypeUser||!IsValidSid(sid.data()))
    throw std::runtime_error("console_transfer_identity_unavailable");
  sid.resize(sidSize);return sid;
}
inline ConsoleTransferObservation observeConsoleTransfer(const DisplayState& display,bool native){
  ConsoleTransferObservation s;s.administrator=elevated();s.native=native;s.installed=display.installed;s.started=display.deviceReady;s.restart=display.rebootRequired;
  if(!native)return s;
  if(!ProcessIdToSessionId(GetCurrentProcessId(),&s.source))return s;s.console=WTSGetActiveConsoleSessionId();
  if(!s.source||s.source==0xffffffff)return s;
  try{
    ConsoleTransferWtsBuffer protocol;protocol.query(s.source,WTSClientProtocolType);
    if(protocol.size!=sizeof(USHORT))return s;s.protocol=*reinterpret_cast<const USHORT*>(protocol.value);
    ConsoleTransferWtsBuffer state;state.query(s.source,WTSConnectState);
    if(state.size!=sizeof(WTS_CONNECTSTATE_CLASS))return s;s.sourceActive=*reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(state.value)==WTSActive;s.sourceDisconnected=*reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(state.value)==WTSDisconnected;
    WinHandle token;DWORD size=0,tokenSession=0;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)||!GetTokenInformation(token.value,TokenSessionId,&tokenSession,sizeof(tokenSession),&size)||tokenSession!=s.source)return s;
    GetTokenInformation(token.value,TokenUser,nullptr,0,&size);if(!size||size>8192)return s;
    std::vector<BYTE> user(size);if(!GetTokenInformation(token.value,TokenUser,user.data(),size,&size))return s;
    bool occupied=false;const auto sourceSid=consoleTransferAccountSid(s.source,occupied);
    s.sourceIdentity=occupied&&!sourceSid.empty()&&EqualSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,const_cast<BYTE*>(sourceSid.data()));
    if(!s.console||s.console==0xffffffff)return s;
    const auto consoleSid=consoleTransferAccountSid(s.console,s.consoleOccupied);s.consoleUserKnown=true;
    s.consoleSameUser=s.consoleOccupied&&!sourceSid.empty()&&EqualSid(const_cast<BYTE*>(sourceSid.data()),const_cast<BYTE*>(consoleSid.data()));
  }catch(...){/* Keep unknown identity distinct from an empty console. */}
  return s;
}
inline std::string publicConsoleTransfer(const ConsoleTransferObservation& s){
  const auto reason=consoleTransferReason(s);
  return "{\"supported\":"+std::string(s.native?"true":"false")+",\"eligible\":"+(*reason?"false":"true")+
    ",\"reason\":"+jsonQuote(reason)+",\"sourceSessionId\":"+std::to_string(s.source)+",\"consoleSessionId\":"+std::to_string(s.console)+"}";
}
inline ConsoleTransferResult requestOwnConsoleTransfer(DWORD expectedSource,DWORD expectedConsole){
  requireAdmin();
  return transferOwnConsole({[]{return observeConsoleTransfer(virtualDisplayStatus(),nativeDisplaySelected());},
    [](DWORD source,DWORD console){wchar_t password[]=L"";return WTSConnectSessionW(source,console,password,FALSE)!=FALSE;},
    []{Sleep(100);}},expectedSource,expectedConsole);
}
}
