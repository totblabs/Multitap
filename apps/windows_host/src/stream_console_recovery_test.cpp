#include "stream_console_recovery.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<BYTE> sid(const wchar_t* text=L"S-1-5-21-100-200-300-1001"){PSID raw=nullptr;require(ConvertStringSidToSidW(text,&raw)!=FALSE,"QA SID creation failed");const auto length=GetLengthSid(raw);std::vector<BYTE> bytes(length);CopySid(length,bytes.data(),raw);LocalFree(raw);return bytes;}
StreamConsoleObservation fixture(){StreamConsoleObservation s;s.native=s.installed=s.started=s.complete=s.consoleIdentityKnown=true;s.console=1;s.configuredSid=sid();StreamConsoleSession row;row.binding={2,{99,-1},s.configuredSid};row.identityKnown=true;row.state=WTSDisconnected;row.protocol=2;s.sessions.push_back(row);return s;}
}
int main(){try{unsigned cases=0;
  const auto original=fixture();const auto selected=planStreamConsoleRecovery(original);
  // First install/reboot: storing an account validates a network logon, not an
  // interactive desktop. The client can hand off an existing matching login
  // after RDP disconnects, but must never invent one or bypass a locked console.
  {auto s=original;s.sessions.clear();s.configuredSid.clear();
    auto p=planStreamConsoleRecovery(s);require(!p.ready&&!p.eligible&&p.reason=="windows_identity_required","Fresh install without an account offered a console handoff");++cases;
    s.configuredSid=original.configuredSid;p=planStreamConsoleRecovery(s);const auto summary=JsonParser::parse(streamConsoleDiagnosticSummary(s));
    require(!p.ready&&!p.eligible&&p.reason=="windows_login_required"&&summary.get("configuredIdentity").boolean()&&!summary.get("consoleOccupied").boolean()&&summary.get("activeRdpSessions").number()==0&&summary.get("disconnectedRdpSessions").number()==0,"Saving credentials was mistaken for a Windows interactive login");++cases;
    s.sessions=original.sessions;s.sessions[0].state=WTSActive;p=planStreamConsoleRecovery(s);
    require(p.eligible&&!p.ready&&p.reason=="rdp_session_active","Explicit matching RDP login was not eligible for client handoff");++cases;
    s.sessions[0].state=WTSDisconnected;p=planStreamConsoleRecovery(s);require(p.eligible&&!p.ready&&p.reason=="rdp_session_disconnected","RDP disconnect lost the first-install login");++cases;
    s.configuredSid.clear();p=planStreamConsoleRecovery(s);require(!p.eligible&&!p.ready&&p.reason=="windows_identity_required","Cleared account retained old disconnected-login authority");++cases;
    s.configuredSid=sid(L"S-1-5-21-100-200-300-1002");p=planStreamConsoleRecovery(s);require(!p.eligible&&!p.ready&&p.reason=="windows_identity_mismatch","Changed account inherited a previous account's disconnected login");++cases;
    s.configuredSid=original.configuredSid;s.sessions[0].state=WTSActive;s.sessions[0].protocol=0;s.console=s.sessions[0].binding.session;s.consoleOccupied=true;s.consoleSid=s.configuredSid;s.sessions[0].unlocked=true;
    p=planStreamConsoleRecovery(s);require(p.ready&&!p.eligible&&sameStreamConsoleBinding(p.source,original.sessions[0].binding),"Fresh verification after handoff lost the exact Windows login");++cases;
    s.sessions[0].unlocked=false;p=planStreamConsoleRecovery(s);require(!p.ready&&!p.eligible&&p.reason=="windows_session_locked","First-install recovery bypassed console lock");++cases;
  }
  {auto observed=original;auto disconnected=observed.sessions[0];disconnected.protocol=0;observed.sessions.push_back(disconnected);auto active=disconnected;active.protocol=2;active.state=WTSActive;active.unlocked=true;observed.sessions.push_back(active);auto unknown=disconnected;unknown.protocol=USHRT_MAX;unknown.identityKnown=false;observed.sessions.push_back(unknown);
    const auto summary=JsonParser::parse(streamConsoleDiagnosticSummary(observed));require(summary.get("activeRdpSessions").number()==1&&summary.get("disconnectedRdpSessions").number()==1&&summary.get("disconnectedConsoleSessions").number()==1&&summary.get("unknownSessions").number()==1&&summary.get("lockedSessions").number()==2,"Bounded WTS category counts were incorrect");require(streamConsoleDiagnosticSummary(observed).find("S-1-")==std::string::npos&&std::get<Json::Object>(summary.value).size()==12,"Console diagnostics exposed Windows identity or unexpected fields");cases+=2;
  }
  require(selected.eligible&&!selected.ready&&selected.source.session==2&&selected.console==1&&selected.reason=="rdp_session_disconnected","Disconnected registered Windows login was mistaken for logout");++cases;
  {auto s=original;s.sessions[0].state=WTSActive;require(planStreamConsoleRecovery(s).eligible&&planStreamConsoleRecovery(s).reason=="rdp_session_active","Active RDP login did not offer an explicit handoff");++cases;}
  for(int scenario=0;scenario<6;++scenario){auto s=original;s.sessions[0].protocol=0;
    if(scenario==1)s.sessions[0].identityKnown=false;
    if(scenario==2)s.sessions[0].binding.sid=sid(L"S-1-5-21-100-200-300-1002");
    if(scenario==3){s.sessions.push_back(s.sessions[0]);s.sessions[1].binding.session=3;s.sessions[1].protocol=2;}
    if(scenario==4)s.sessions[0].protocol=USHRT_MAX;
    if(scenario==5)s.sessions[0].state=WTSActive;
    const char* reasons[]={"rdp_session_disconnected","windows_session_unknown","windows_identity_mismatch","windows_session_ambiguous","windows_session_unknown","windows_session_unknown"};
    const auto p=planStreamConsoleRecovery(s);require(p.eligible==(scenario==0)&&!p.ready&&p.reason==reasons[scenario],"Disconnected or unknown-protocol Windows login was mistaken for logout or wrong identity accepted");++cases;
  }
  {ConsoleTransferObservation s;s.source=2;s.console=1;s.administrator=s.native=s.installed=s.started=s.sourceIdentity=s.consoleUserKnown=s.sourceDisconnected=true;s.protocol=0;
    require(std::string(consoleTransferReason(s))=="active_rdp_session_required"&&std::string(consoleTransferReason(s,true)).empty(),"Existing disconnected console login cannot use the identity-bound recovery path");++cases;
  }
  {auto s=original;s.sessions[0].protocol=0;s.sessions[0].binding.session=s.console;s.sessions[0].state=WTSActive;s.sessions[0].unlocked=true;s.configuredSid.clear();const auto p=planStreamConsoleRecovery(s);require(p.ready&&!p.eligible&&p.reason.empty(),"Existing console required stored Windows credentials or needless transfer");s.sessions[0].unlocked=false;require(planStreamConsoleRecovery(s).reason=="windows_session_locked"&&!planStreamConsoleRecovery(s).eligible,"Locked console was silently resumed");cases+=2;}
  const std::vector<const char*> expected{"native_provider_required","reboot_required","virtual_device_not_ready","virtual_device_not_ready","windows_session_unknown","windows_identity_required","console_unavailable","console_unavailable","windows_session_unknown","console_in_use","windows_session_unknown","windows_identity_mismatch","windows_login_required","windows_login_required","console_unavailable","windows_session_ambiguous","windows_session_unknown"};
  for(size_t i=0;i<expected.size();++i){auto s=original;switch(i){
    case 0:s.native=false;break;case 1:s.restart=true;break;case 2:s.installed=false;break;case 3:s.started=false;break;case 4:s.complete=false;break;case 5:s.configuredSid.clear();break;case 6:s.console=0;break;case 7:s.console=0xffffffff;break;case 8:s.consoleIdentityKnown=false;break;
    case 9:s.consoleOccupied=true;s.consoleSid=sid(L"S-1-5-21-100-200-300-1002");break;case 10:s.sessions[0].identityKnown=false;break;case 11:s.sessions[0].binding.sid=sid(L"S-1-5-21-100-200-300-1002");break;case 12:s.sessions.clear();break;case 13:s.sessions[0].state=WTSListen;break;case 14:s.console=s.sessions[0].binding.session;break;case 15:s.sessions.push_back(s.sessions[0]);s.sessions[1].binding.session=3;break;case 16:s.sessions.resize(65);break;
  }const auto p=planStreamConsoleRecovery(s);require(!p.ready&&!p.eligible&&p.reason==expected[i],"Unverified or unrelated Windows session reached transfer planning");++cases;}
  {auto s=original;s.consoleOccupied=true;s.consoleSid=s.configuredSid;require(planStreamConsoleRecovery(s).eligible,"Same user's console was treated as another account");++cases;}
  {auto s=original;auto foreign=s.sessions[0];foreign.binding.session=3;foreign.binding.sid=sid(L"S-1-5-21-100-200-300-1002");foreign.state=WTSActive;s.sessions.push_back(foreign);const auto p=planStreamConsoleRecovery(s);require(p.eligible&&p.source.session==2&&p.reason=="rdp_session_disconnected","Another account's active RDP replaced selected disconnected login");++cases;}
  for(int change=0;change<7;++change){auto s=original;switch(change){case 0:s.console=3;break;case 1:s.sessions[0].binding.session=3;break;case 2:s.sessions[0].binding.logon.LowPart++;break;case 3:s.sessions[0].binding.logon.HighPart++;break;case 4:s.sessions[0].binding.sid=sid(L"S-1-5-21-100-200-300-1002");break;case 5:s.configuredSid=sid(L"S-1-5-21-100-200-300-1002");break;case 6:s.complete=false;break;}require(!sameStreamConsolePlan(selected,planStreamConsoleRecovery(s)),"Changed source/console/identity/logon retained old handoff authority");++cases;}
  {const auto encoded=streamConsoleBindingJson(selected.source);const auto decoded=parseStreamConsoleBinding(JsonParser::parse(encoded));require(sameStreamConsoleBinding(selected.source,decoded),"Private binding roundtrip lost exact Windows logon identity");++cases;}
  for(int change=0;change<9;++change){auto value=JsonParser::parse(streamConsoleBindingJson(selected.source));auto& fields=std::get<Json::Object>(value.value);switch(change){case 0:fields["sessionId"].value=0.0;break;case 1:fields["sessionId"].value=4294967295.0;break;case 2:fields["authenticationLow"].value=-1.0;break;case 3:fields["authenticationHigh"].value=2147483648.0;break;case 4:fields["sessionId"].value=2.5;break;case 5:fields["userSid"].value=std::string("S-NOT-A-SID");break;case 6:fields["userSid"].value=std::string(185,'a');break;case 7:fields["extra"].value=true;break;case 8:fields["authenticationLow"].value=std::string("99");break;}bool refused=false;try{parseStreamConsoleBinding(value);}catch(...){refused=true;}require(refused,"Malformed private Windows binding accepted");++cases;}
  {auto s=ConsoleTransferObservation{};s.source=2;s.console=1;s.administrator=s.native=s.installed=s.started=s.sourceIdentity=s.consoleUserKnown=s.sourceDisconnected=true;s.protocol=2;require(std::string(consoleTransferReason(s))=="active_rdp_session_required"&&std::string(consoleTransferReason(s,true)).empty(),"Disconnected policy changed the existing Host settings default");int calls=0;const auto result=transferOwnConsole({[&]{return s;},[&](DWORD,DWORD){++calls;return true;},[]{}},2,1,true);require(result.requested&&!result.completed&&calls==1,"Unconfirmed transfer was retried or advertised completed");cases+=2;}
  std::cout<<"{\"kind\":\"stream-console-recovery-policy\",\"cases\":"<<cases<<",\"registeredWindowsSidAndLogonBinding\":true,\"disconnectedDistinguishedFromLogout\":true,\"lockedConsoleNotBypassed\":true,\"multipleLoginsRefused\":true,\"actualWtsTransfers\":0,\"credentialStorageChanged\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
