#pragma once
#include "console_transfer.hpp"
#include "windows_account_record.hpp"
#include "display_operation.hpp"
#include "worker_token.hpp"
#include <climits>
#include <functional>
#include <set>

namespace host {
// Only a configured Windows identity's existing login can be resumed. No
// Windows usernames, SIDs, logon IDs or passwords go to the broker/client.
struct StreamConsoleBinding {
  DWORD session=0;LUID logon{};std::vector<BYTE> sid;
};
inline bool sameStreamConsoleBinding(const StreamConsoleBinding& a,const StreamConsoleBinding& b){
  return a.session==b.session&&a.session&&a.session!=0xffffffff&&a.logon.LowPart==b.logon.LowPart&&a.logon.HighPart==b.logon.HighPart&&!a.sid.empty()&&a.sid==b.sid;
}
struct StreamConsoleSession {
  StreamConsoleBinding binding;WTS_CONNECTSTATE_CLASS state=WTSInit;USHORT protocol=USHRT_MAX;
  bool identityKnown=false,unlocked=false;
};
struct StreamConsoleObservation {
  DWORD console=0xffffffff;bool native=false,installed=false,started=false,restart=false,complete=false;
  bool consoleIdentityKnown=false,consoleOccupied=false;std::vector<BYTE> consoleSid,configuredSid;
  std::vector<StreamConsoleSession> sessions;
};
struct StreamConsolePlan {
  StreamConsoleBinding source;DWORD console=0xffffffff;std::string reason="windows_session_unknown";
  bool ready=false,eligible=false;
};
inline StreamConsolePlan planStreamConsoleRecovery(const StreamConsoleObservation& s){
  StreamConsolePlan result;result.console=s.console;
  const auto fail=[&](const char* reason){result.reason=reason;return result;};
  if(!s.native)return fail("native_provider_required");
  if(s.restart)return fail("reboot_required");
  if(!s.installed||!s.started)return fail("virtual_device_not_ready");
  if(!s.complete||s.sessions.size()>64)return fail("windows_session_unknown");
  // A currently unlocked console needs no transfer and no stored password.
  const auto active=std::find_if(s.sessions.begin(),s.sessions.end(),[&](const auto& row){return row.binding.session==s.console&&row.state==WTSActive&&row.protocol==0;});
  if(active!=s.sessions.end()){
    if(!active->identityKnown)return fail("windows_session_unknown");
    if(!active->unlocked)return fail("windows_session_locked");
    result.source=active->binding;result.ready=true;result.reason.clear();return result;
  }
  if(s.configuredSid.empty())return fail("windows_identity_required");
  if(!s.console||s.console==0xffffffff)return fail("console_unavailable");
  if(!s.consoleIdentityKnown)return fail("windows_session_unknown");
  if(s.consoleOccupied&&s.consoleSid!=s.configuredSid)return fail("console_in_use");
  size_t matches=0;bool rdpActive=false,disconnected=false,selectedActive=false;
  for(const auto& row:s.sessions){
    if(!row.binding.session||row.binding.session==0xffffffff)continue;
    if(row.protocol==2&&row.state==WTSActive)rdpActive=true;
    if(row.state==WTSDisconnected)disconnected=true;
    if(row.state!=WTSActive&&row.state!=WTSDisconnected)continue;
    // A disconnected login is still a login even when it reports the local
    // protocol. Admit only a known local/RDP protocol and the registered SID;
    // an unresolved/other active session must never imply that nobody logged in.
    if((row.protocol!=0&&row.protocol!=2)||(row.state==WTSActive&&row.protocol!=2))return fail("windows_session_unknown");
    // An unresolved active/disconnected login could be the requested account.
    if(!row.identityKnown)return fail("windows_session_unknown");
    if(row.binding.sid!=s.configuredSid)continue;
    if(row.binding.session==s.console)return fail("console_unavailable");
    result.source=row.binding;selectedActive=row.state==WTSActive;++matches;
  }
  if(matches>1)return fail("windows_session_ambiguous");
  if(!matches)return fail(rdpActive||disconnected?"windows_identity_mismatch":"windows_login_required");
  result.eligible=true;result.reason=selectedActive?"rdp_session_active":"rdp_session_disconnected";return result;
}
inline bool sameStreamConsolePlan(const StreamConsolePlan& expected,const StreamConsolePlan& fresh){
  return expected.eligible&&fresh.eligible&&expected.console==fresh.console&&sameStreamConsoleBinding(expected.source,fresh.source);
}
inline StreamConsoleBinding streamConsoleTokenBinding(HANDLE token,DWORD expectedSession){
  StreamConsoleBinding binding;DWORD bytes=0;
  if(!GetTokenInformation(token,TokenSessionId,&binding.session,sizeof(binding.session),&bytes)||binding.session!=expectedSession||!expectedSession||expectedSession==0xffffffff)throw std::runtime_error("windows_session_unknown");
  TOKEN_STATISTICS statistics{};
  if(!GetTokenInformation(token,TokenStatistics,&statistics,sizeof(statistics),&bytes))throw std::runtime_error("windows_session_unknown");binding.logon=statistics.AuthenticationId;
  GetTokenInformation(token,TokenUser,nullptr,0,&bytes);if(!bytes||bytes>8192)throw std::runtime_error("windows_session_unknown");
  std::vector<BYTE> user(bytes);if(!GetTokenInformation(token,TokenUser,user.data(),bytes,&bytes))throw std::runtime_error("windows_session_unknown");
  const auto sid=reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;if(!IsValidSid(sid))throw std::runtime_error("windows_session_unknown");
  const DWORD length=GetLengthSid(sid);if(!length||length>SECURITY_MAX_SID_SIZE)throw std::runtime_error("windows_session_unknown");
  binding.sid.resize(length);if(!CopySid(length,binding.sid.data(),sid))throw std::runtime_error("windows_session_unknown");return binding;
}
inline std::vector<BYTE> configuredStreamConsoleSid(){
  const auto credentials=loadWindowsAccount();if(!credentials)return {};
  std::wstring account=credentials->enteredName;
  if(credentials->domain==L"."){wchar_t computer[MAX_COMPUTERNAME_LENGTH+1]{};DWORD length=DWORD(std::size(computer));if(!GetComputerNameW(computer,&length))return {};account=std::wstring(computer)+L"\\"+credentials->username;}
  DWORD sidSize=0,domainSize=0;SID_NAME_USE use{};LookupAccountNameW(nullptr,account.c_str(),nullptr,&sidSize,nullptr,&domainSize,&use);
  if(!sidSize||sidSize>SECURITY_MAX_SID_SIZE||domainSize>1024)return {};
  std::vector<BYTE> sid(sidSize);std::wstring domain(std::max<DWORD>(1,domainSize),L'\0');
  if(!LookupAccountNameW(nullptr,account.c_str(),sid.data(),&sidSize,domain.data(),&domainSize,&use)||use!=SidTypeUser||!IsValidSid(sid.data()))return {};
  sid.resize(sidSize);return sid;
}
inline bool streamConsoleUnlocked(DWORD session){
  try{ConsoleTransferWtsBuffer info;info.query(session,WTSSessionInfoEx);if(info.size<sizeof(WTSINFOEXW))return false;
    const auto* value=reinterpret_cast<const WTSINFOEXW*>(info.value);return value->Level==1&&value->Data.WTSInfoExLevel1.SessionFlags==WTS_SESSIONSTATE_UNLOCK;
  }catch(...){return false;}
}
// The Windows identities a plan compares: the stored account (a DPAPI read and
// two LSA lookups), the console's user (two WTS reads, two LSA lookups) and every
// signed-in session (WTS reads and its token): ~20-25 wakeups in the service,
// nearly all of an observation. The capability report keeps them until a session
// change, a new stored account or 60 s (capability_inputs.hpp); an operation, the
// diagnostics report and the console recovery itself read them afresh.
struct StreamConsoleIdentity {
  std::vector<BYTE> configuredSid,consoleSid;bool consoleIdentityKnown=false,consoleOccupied=false,complete=false;
  std::vector<StreamConsoleSession> sessions;
};
inline StreamConsoleIdentity observeStreamConsoleIdentity(DWORD console){
  StreamConsoleIdentity result;
  try{result.configuredSid=configuredStreamConsoleSid();}catch(...){ }
  try{result.consoleSid=consoleTransferAccountSid(console,result.consoleOccupied);result.consoleIdentityKnown=true;}catch(...){ }
  struct Sessions{WTS_SESSION_INFOW* value=nullptr;~Sessions(){if(value)WTSFreeMemory(value);}}sessions;DWORD count=0;
  if(!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&sessions.value,&count)||count>64)return result;
  for(DWORD i=0;i<count;++i){const auto& entry=sessions.value[i];if(!entry.SessionId||(entry.State!=WTSActive&&entry.State!=WTSDisconnected))continue;
    StreamConsoleSession row;row.binding.session=entry.SessionId;row.state=entry.State;
    try{ConsoleTransferWtsBuffer protocol;protocol.query(entry.SessionId,WTSClientProtocolType);if(protocol.size!=sizeof(USHORT))throw std::runtime_error("windows_session_unknown");row.protocol=*reinterpret_cast<const USHORT*>(protocol.value);
      const bool occupied=!consoleTransferAccountText(entry.SessionId,WTSUserName).empty();
      if(!occupied)continue;
      // Bind the same selected primary token used by UserWorker, including a
      // split administrator's linked token. Do not assume a different token's
      // authentication LUID is interchangeable with the created process.
      auto token=interactiveWorkerToken(entry.SessionId);row.binding=streamConsoleTokenBinding(token.value,entry.SessionId);row.identityKnown=true;row.unlocked=streamConsoleUnlocked(entry.SessionId);
    }catch(...){ }
    result.sessions.push_back(std::move(row));
  }
  result.complete=true;return result;
}
// `identity`: where the Windows identities come from (fresh by default).
inline StreamConsoleObservation observeStreamConsoleRecovery(const std::function<StreamConsoleIdentity(DWORD)>& identity=observeStreamConsoleIdentity){
  StreamConsoleObservation result;result.native=nativeDisplaySelected();if(!result.native)return result;
  const auto device=virtualDisplayStatus();result.installed=device.installed;result.started=device.deviceReady;result.restart=device.rebootRequired;result.console=WTSGetActiveConsoleSessionId();
  auto value=identity(result.console);
  result.configuredSid=std::move(value.configuredSid);result.consoleSid=std::move(value.consoleSid);result.consoleIdentityKnown=value.consoleIdentityKnown;result.consoleOccupied=value.consoleOccupied;
  result.sessions=std::move(value.sessions);result.complete=value.complete;return result;
}
inline std::string streamConsoleBindingJson(const StreamConsoleBinding& binding){
  if(!binding.session||binding.session==0xffffffff||binding.sid.size()<8||binding.sid[0]!=SID_REVISION||binding.sid[1]>SID_MAX_SUB_AUTHORITIES||binding.sid.size()!=8+size_t(binding.sid[1])*4||!IsValidSid(const_cast<BYTE*>(binding.sid.data())))throw std::runtime_error("windows_session_unknown");
  LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(const_cast<BYTE*>(binding.sid.data()),&sid))throw std::runtime_error("windows_session_unknown");
  const auto text=utf8(sid);LocalFree(sid);
  return "{\"sessionId\":"+std::to_string(binding.session)+",\"authenticationLow\":"+std::to_string(binding.logon.LowPart)+",\"authenticationHigh\":"+std::to_string(binding.logon.HighPart)+",\"userSid\":"+jsonQuote(text)+'}';
}
inline StreamConsoleBinding parseStreamConsoleBinding(const Json& value){
  const auto* object=std::get_if<Json::Object>(&value.value);const auto session=value.get("sessionId").number(-1),low=value.get("authenticationLow").number(-1),high=value.get("authenticationHigh").number(double(LONG_MIN)-1);
  const auto text=value.get("userSid").string();
  const auto integer=[](double n,double minimum,double maximum){return std::isfinite(n)&&n>=minimum&&n<=maximum&&std::floor(n)==n;};
  if(!object||object->size()!=4||!integer(session,1,UINT32_MAX-1)||!integer(low,0,UINT32_MAX)||!integer(high,LONG_MIN,LONG_MAX)||text.empty()||text.size()>184)throw std::runtime_error("windows_session_unknown");
  PSID raw=nullptr;if(!ConvertStringSidToSidW(wide(text).c_str(),&raw))throw std::runtime_error("windows_session_unknown");
  struct Sid{PSID value;~Sid(){LocalFree(value);}}sid{raw};
  const DWORD length=IsValidSid(raw)?GetLengthSid(raw):0;if(!length||length>SECURITY_MAX_SID_SIZE)throw std::runtime_error("windows_session_unknown");
  StreamConsoleBinding result;result.session=DWORD(session);result.logon={DWORD(low),LONG(high)};result.sid.resize(length);if(!CopySid(length,result.sid.data(),raw))throw std::runtime_error("windows_session_unknown");return result;
}
inline StreamConsoleBinding ownStreamConsoleBinding(){
  WinHandle token;DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))throw std::runtime_error("windows_session_unknown");return streamConsoleTokenBinding(token.value,session);
}
inline std::string safeStreamConsoleReason(const std::string& reason){
  static const std::set<std::string> allowed{"native_provider_required","reboot_required","virtual_device_not_ready","windows_session_unknown","windows_session_locked","windows_identity_required","windows_identity_mismatch","windows_login_required","windows_session_ambiguous","rdp_session_active","rdp_session_disconnected","console_unavailable","console_in_use","operation_cancelled","console_busy","console_timeout","console_transfer_failed","console_transfer_pending","console_binding_changed","worker_failed","administrator_required"};
  return allowed.count(reason)?reason:"worker_failed";
}
inline std::string streamConsoleDiagnosticSummary(const StreamConsoleObservation& observed){
  const auto plan=planStreamConsoleRecovery(observed);unsigned activeRdp=0,disconnectedRdp=0,disconnectedConsole=0,unknown=0,locked=0;
  if(observed.sessions.size()>64)throw std::runtime_error("windows_session_unknown");
  for(const auto& session:observed.sessions){
    if(session.protocol==2&&session.state==WTSActive)++activeRdp;
    if(session.protocol==2&&session.state==WTSDisconnected)++disconnectedRdp;
    if(session.protocol==0&&session.state==WTSDisconnected)++disconnectedConsole;
    if(!session.identityKnown||(session.protocol!=0&&session.protocol!=2)||(session.state!=WTSActive&&session.state!=WTSDisconnected))++unknown;
    if(session.identityKnown&&!session.unlocked)++locked;
  }
  const auto bit=[](bool value){return value?"true":"false";};
  return "{\"reason\":"+jsonQuote(plan.reason.empty()?"":safeStreamConsoleReason(plan.reason))+",\"ready\":"+bit(plan.ready)+",\"eligible\":"+bit(plan.eligible)+",\"complete\":"+bit(observed.complete)+",\"configuredIdentity\":"+bit(!observed.configuredSid.empty())+",\"consoleIdentityKnown\":"+bit(observed.consoleIdentityKnown)+",\"consoleOccupied\":"+bit(observed.consoleOccupied)+",\"activeRdpSessions\":"+std::to_string(activeRdp)+",\"disconnectedRdpSessions\":"+std::to_string(disconnectedRdp)+",\"disconnectedConsoleSessions\":"+std::to_string(disconnectedConsole)+",\"unknownSessions\":"+std::to_string(unknown)+",\"lockedSessions\":"+std::to_string(locked)+'}';
}
}
