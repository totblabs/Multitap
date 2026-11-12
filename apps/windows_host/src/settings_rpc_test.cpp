#include "settings_rpc.hpp"
#include "setup_pnp.hpp"
#include "installed_host_layout.hpp"
#include <map>
#include <iostream>
using namespace host;
namespace {
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
// display-adapters-v1: what WorkPC (pve-247) shows: Citrix, Microsoft Basic, our
// current node, four unlabelled libvirtualdisplay nodes, and a legacy MttVDD node.
NativeConsoleAdapterIdentity adapterNode(std::wstring id,std::wstring enumerator,std::vector<std::wstring> hardware,bool started=true){
  NativeConsoleAdapterIdentity node;node.instanceId=std::move(id);node.enumerator=std::move(enumerator);node.hardwareIds=std::move(hardware);node.classGuid=nativeConsoleDisplayClass;node.present=true;node.started=started;return node;
}
std::vector<NativeConsoleAdapterIdentity> workPcAdapters(){
  return {adapterNode(L"ROOT\\DISPLAY\\0000",L"ROOT",{L"Citrix_IDD"}),adapterNode(L"PCI\\VEN_1234&DEV_1111\\3&1",L"PCI",{L"PCI\\VEN_1234&DEV_1111"}),
    adapterNode(L"ROOT\\DISPLAY\\0004",L"ROOT",{L"Root\\SunshineVirtualDisplay"}),
    adapterNode(L"ROOT\\DISPLAY\\0001",L"ROOT",{L"Root\\SunshineVirtualDisplay"},false),adapterNode(L"ROOT\\DISPLAY\\0002",L"ROOT",{L"Root\\SunshineVirtualDisplay"}),
    adapterNode(L"ROOT\\DISPLAY\\0003",L"ROOT",{L"Root\\SunshineVirtualDisplay"}),adapterNode(L"ROOT\\DISPLAY\\0005",L"ROOT",{L"Root\\SunshineVirtualDisplay"}),
    adapterNode(L"ROOT\\MULTITAPVIRTUALDISPLAY\\0000",L"ROOT",{L"Root\\MttVDD"})};
}
int displayAdapterOwnership(){
  int cases=0;
  DisplayAdapterOwnership owner;owner.current=L"ROOT\\DISPLAY\\0004";owner.legacyOwner=L"ROOT\\MULTITAPVIRTUALDISPLAY\\0000";owner.created={L"ROOT\\DISPLAY\\0002"};
  auto inventory=summarizeDisplayAdapters(workPcAdapters(),owner);
  require(inventory.current==1&&inventory.currentStarted&&inventory.legacy==1&&inventory.duplicate==1&&inventory.unproven==3&&inventory.foreign==2,"WorkPC adapters were sorted wrongly");++cases;
  require(inventory.extras.size()==5&&std::none_of(inventory.extras.begin(),inventory.extras.end(),[](const auto& e){return e.instance==L"ROOT\\DISPLAY\\0000"||e.instance==L"ROOT\\DISPLAY\\0004";}),"A current or foreign adapter was offered for removal");++cases;
  // A name-shaped Citrix node, a two-ID node, another class: never ours.
  {auto node=adapterNode(L"ROOT\\DISPLAY\\0009",L"ROOT",{L"Root\\SunshineVirtualDisplay",L"Root\\Other"});require(classifyDisplayAdapter(node,owner)==DisplayAdapterKind::foreign,"A node with an extra hardware ID counted as ours");++cases;}
  {auto node=adapterNode(L"ROOT\\DISPLAY\\0009",L"ROOT",{L"Root\\SunshineVirtualDisplay"});node.classGuid=GUID{};require(classifyDisplayAdapter(node,owner)==DisplayAdapterKind::foreign,"Another device class counted as ours");++cases;}
  {auto node=adapterNode(L"SWD\\DISPLAY\\0009",L"SWD",{L"Root\\SunshineVirtualDisplay"});require(classifyDisplayAdapter(node,owner)==DisplayAdapterKind::foreign,"A non-root node counted as ours");++cases;}
  // Without the owner marker the legacy-shaped node is only unproven; without a selection nothing is current.
  {DisplayAdapterOwnership bare;const auto plain=summarizeDisplayAdapters(workPcAdapters(),bare);require(plain.current==0&&plain.legacy==0&&plain.duplicate==0&&plain.unproven==6,"Ownership was assumed without proof");++cases;}
  // A phantom of ours is still ours; the ledger is case-insensitive.
  {DisplayAdapterOwnership upper=owner;upper.created={L"root\\display\\0001"};auto node=adapterNode(L"ROOT\\DISPLAY\\0001",L"ROOT",{L"Root\\SunshineVirtualDisplay"},false);node.present=false;require(classifyDisplayAdapter(node,upper)==DisplayAdapterKind::duplicate,"A recorded phantom was not ours");++cases;}
  // Automatic cleanup only with the runtime's own adapter started and nothing going on.
  require(*displayAdapterCleanupBlocker(inventory,true,true,false)==0,"Cleanup was blocked with every gate open");++cases;
  require(std::string(displayAdapterCleanupBlocker(inventory,false,true,false))=="current_not_ready"&&std::string(displayAdapterCleanupBlocker(inventory,true,false,false))=="busy"&&
    std::string(displayAdapterCleanupBlocker(inventory,true,true,true))=="display_state_pending","A closed gate let cleanup run");++cases;
  {auto stopped=inventory;stopped.currentStarted=false;require(std::string(displayAdapterCleanupBlocker(stopped,true,true,false))=="current_not_ready","Cleanup ran while the runtime adapter was not started");++cases;}
  {DisplayAdapterInventory none;none.read=true;none.unproven=4;require(*displayAdapterCleanupBlocker(none,false,false,true)==0,"Nothing provable to remove still reported a blocker");++cases;}
  require(std::string(displayAdapterCleanupBlocker(DisplayAdapterInventory{},true,true,false))=="inventory_unavailable","An unread inventory allowed cleanup");++cases;
  // Diagnostics: counts and fixed codes only, re-encoded or dropped.
  const auto json=displayAdapterInventoryJson(inventory,"busy",DisplayAdapterCleanup::pending);
  require(json.find("ROOT\\")==std::string::npos&&json.find("DISPLAY")==std::string::npos,"An instance ID reached the diagnostics");
  const auto devices=hostDiagnosticDevices("{\"schema\":1,\"displayAdapters\":"+json+",\"usbController\":\"foreign\"}");
  const auto parsed=JsonParser::parse("{"+devices.substr(1)+"}").get("devices");
  require(parsed.get("displayAdapters").get("unproven").number()==3&&parsed.get("displayAdapters").get("cleanup").string()=="pending_restart"&&parsed.get("displayAdapters").get("blocker").string()=="busy"&&parsed.get("usbController").string()=="foreign","The devices diagnostic lost its counts or codes");++cases;
  require(hostDiagnosticDevices("{\"schema\":1,\"displayAdapters\":{\"read\":false},\"usbController\":\"C:\\\\private\"}").empty()&&hostDiagnosticDevices("{\"schema\":2}").empty()&&hostDiagnosticDevices("").empty(),"An unknown devices diagnostic was passed on");++cases;
  // The setup's ledger and the service read the same key.
  require(std::wstring(multitap::pnp::OwnedNodesKey)==nativeAdapterLedgerKey,"Setup and service disagree on the adapter ledger");++cases;
  return cases;
}
// 정리 in Host settings: exactly the confirmed list, re-proved per node.
int displayAdapterCleanupRequest(){
  int cases=0;
  DisplayAdapterOwnership owner;owner.current=L"ROOT\\DISPLAY\\0004";
  const auto inventory=summarizeDisplayAdapters(workPcAdapters(),owner);std::vector<std::wstring> asked;
  AdapterCleanupAccess access;access.inventory=[&]{return std::optional(inventory);};access.remove=[&](const DisplayAdapterExtra& e){asked.push_back(e.instance);return e.instance==L"ROOT\\DISPLAY\\0003"?DisplayAdapterCleanup::pending:DisplayAdapterCleanup::removed;};
  std::vector<std::wstring> all;for(const auto& e:inventory.extras)all.push_back(e.instance);
  auto value=JsonParser::parse(cleanDisplayAdaptersResult(access,all));
  require(value.get("error").string().empty()&&value.get("removed").number()==4&&value.get("pending").number()==1&&asked.size()==5,"The confirmed extras were not all removed");++cases;
  asked.clear();auto shorter=all;shorter.pop_back();
  require(JsonParser::parse(cleanDisplayAdaptersResult(access,shorter)).get("error").string()=="display_adapters_changed"&&asked.empty(),"A list that no longer matched was acted on");++cases;
  auto swapped=all;swapped.back()=L"ROOT\\DISPLAY\\0004";
  require(JsonParser::parse(cleanDisplayAdaptersResult(access,swapped)).get("error").string()=="display_adapters_changed"&&asked.empty(),"The current adapter could be confirmed into removal");++cases;
  access.inventory=[]{return std::optional<DisplayAdapterInventory>();};
  require(JsonParser::parse(cleanDisplayAdaptersResult(access,all)).get("error").string()=="display_adapters_unavailable","An unreadable inventory was acted on");++cases;
  return cases;
}
// Multitap용으로 바꾸기: only another product's controller, only confirmed.
int usbControllerReplacement(){
  int cases=0;
  struct Fake{usb::ControllerOwner owner=usb::ControllerOwner::foreign;int removed=1;bool removeRestart=false;DWORD installError=0;bool installRestart=false;int after=1;std::vector<std::string> log;};
  const auto access=[](Fake& f){usb::ReplaceAccess a;a.platformSupported=[]{return true;};a.packagePresent=[]{return true;};a.packageVerified=[]{return true;};
    a.owner=[&f]{return f.owner;};a.removeOthers=[&f](bool& restart){f.log.push_back("remove");restart=f.removeRestart;return f.removed;};
    a.install=[&f](bool& restart){f.log.push_back("install");restart=f.installRestart;return f.installError;};a.controllerState=[&f]{return f.after;};return a;};
  {Fake f;const auto v=JsonParser::parse(replaceUsbControllerResult(access(f),false));require(v.get("error").string()=="confirmation_required"&&f.log.empty(),"An unconfirmed replacement ran");++cases;}
  {Fake f;const auto v=JsonParser::parse(replaceUsbControllerResult(access(f),true));require(v.get("error").string().empty()&&v.get("removed").number()==1&&f.log==std::vector<std::string>{"remove","install"},"A foreign controller was not replaced");++cases;}
  for(const auto owner:{usb::ControllerOwner::none,usb::ControllerOwner::ours,usb::ControllerOwner::oursStopped,usb::ControllerOwner::unmarked,usb::ControllerOwner::duplicates}){Fake f;f.owner=owner;const auto v=JsonParser::parse(replaceUsbControllerResult(access(f),true));require(v.get("error").string()=="usb_controller_not_foreign"&&f.log.empty(),"Our own controller was replaced");++cases;}
  {Fake f;f.owner=usb::ControllerOwner::multiple;f.removed=2;require(JsonParser::parse(replaceUsbControllerResult(access(f),true)).get("removed").number()==2,"Several foreign controllers were not all replaced");++cases;}
  {Fake f;f.removed=-1;const auto v=JsonParser::parse(replaceUsbControllerResult(access(f),true));require(v.get("error").string()=="usb_replace_remove_failed"&&f.log==std::vector<std::string>{"remove"},"A failed removal went on to install");++cases;}
  {Fake f;f.removeRestart=true;const auto v=JsonParser::parse(replaceUsbControllerResult(access(f),true));require(v.get("error").string()=="usb_restart_required"&&v.get("restart").boolean()&&f.log==std::vector<std::string>{"remove"},"A removal Windows had not finished went on to install");++cases;}
  {Fake f;f.after=2;f.installRestart=true;const auto v=JsonParser::parse(replaceUsbControllerResult(access(f),true));require(v.get("error").string()=="usb_restart_required"&&v.get("restart").boolean(),"A restart Windows asked for was lost");++cases;}
  {Fake f;f.installError=5;require(JsonParser::parse(replaceUsbControllerResult(access(f),true)).get("error").string()=="usb_driver_install_failed","A failed install was reported as done");++cases;}
  return cases;
}
// A first Windows password (windows_account_password.hpp). Every NetUser,
// LookupAccountSid, LogonUser and NetUserChangePassword call is a fake: no real
// account on this machine is read, signed in or changed.
namespace fakenet {
std::wstring name=L"fixture",machineName=L"FIXTURE-PC",sidDomain=L"FIXTURE-PC";
SID_NAME_USE use=SidTypeUser;BOOL linked=FALSE;DWORD flags=UF_NORMAL_ACCOUNT|UF_DONT_EXPIRE_PASSWD;PSID sid=nullptr;int reads=0;
USER_INFO_4 info4{};USER_INFO_24 info24{};
int changes=0;std::wstring changeServer,changeName,oldPassword,newPassword;
std::function<NET_API_STATUS(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR)> onChange;
void reset(){name=L"fixture";machineName=sidDomain=L"FIXTURE-PC";use=SidTypeUser;linked=FALSE;flags=UF_NORMAL_ACCOUNT|UF_DONT_EXPIRE_PASSWD;reads=changes=0;changeServer.clear();changeName.clear();oldPassword.clear();newPassword.clear();onChange={};}
NET_API_STATUS NET_API_FUNCTION change(LPCWSTR server,LPCWSTR user,LPCWSTR oldValue,LPCWSTR newValue){
  ++changes;changeServer=server?server:L"";changeName=user?user:L"";oldPassword=oldValue?oldValue:L"NULL";newPassword=newValue?newValue:L"";
  return onChange?onChange(server,user,oldValue,newValue):ERROR_INVALID_FUNCTION;
}
NET_API_STATUS NET_API_FUNCTION getInfo(LPCWSTR server,LPCWSTR user,DWORD level,LPBYTE* out){
  ++reads;if(server||_wcsicmp(user,name.c_str()))return NERR_UserNotFound;
  if(level==4){info4={};info4.usri4_name=name.data();info4.usri4_user_sid=sid;info4.usri4_flags=flags;info4.usri4_acct_expires=TIMEQ_FOREVER;*out=reinterpret_cast<LPBYTE>(&info4);return NERR_Success;}
  if(level==24){info24={};info24.usri24_internet_identity=linked;*out=reinterpret_cast<LPBYTE>(&info24);return NERR_Success;}
  return ERROR_INVALID_LEVEL;
}
NET_API_STATUS NET_API_FUNCTION modals(LPCWSTR,DWORD,LPBYTE*){return ERROR_ACCESS_DENIED;}
NET_API_STATUS NET_API_FUNCTION release(LPVOID){return NERR_Success;}
BOOL WINAPI lookupSid(LPCWSTR,PSID,LPWSTR user,LPDWORD userLength,LPWSTR domain,LPDWORD domainLength,PSID_NAME_USE sidUse){
  if(!user||!domain||*userLength<=name.size()||*domainLength<=sidDomain.size()){*userLength=DWORD(name.size()+1);*domainLength=DWORD(sidDomain.size()+1);SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
  wcscpy_s(user,*userLength,name.c_str());wcscpy_s(domain,*domainLength,sidDomain.c_str());*sidUse=use;return TRUE;
}
std::wstring machine(){return machineName;}
const autologon::AccountLookup lookup{getInfo,modals,release,lookupSid,machine};
}
int accountPasswordSet(){
  int cases=0;const std::wstring fixtureSid=L"S-1-5-21-1000-2000-3000-1001";
  require(ConvertStringSidToSidW(fixtureSid.c_str(),&fakenet::sid)!=FALSE,"Fixture SID could not be built");
  struct Cleanup{~Cleanup(){LocalFree(fakenet::sid);fakenet::sid=nullptr;}}cleanup;
  const std::string secret="PRIVATE_SENTINEL-Pw1!";
  // Domain and Microsoft accounts are refused by the same lookup the auto-login step uses.
  {fakenet::reset();const auto local=autologon::localAccount(L"fixture",fakenet::lookup);require(local.name==L"fixture"&&local.domain==L"FIXTURE-PC"&&local.sid==fixtureSid&&local.usable,"The local fixture account was not read");++cases;}
  const auto refused=[](const std::wstring& entered){try{(void)autologon::localAccount(entered,fakenet::lookup);}catch(const autologon::Failure& failure){return std::string(failure.what());}return std::string("accepted");};
  for(const auto* entered:{L"CORP\\fixture",L"fixture@corp.example"}){fakenet::reset();require(refused(entered)=="local_account_required"&&fakenet::reads==0,"A domain account name was looked up as local");++cases;}
  {fakenet::reset();fakenet::sidDomain=L"CORP";require(refused(L"fixture")=="local_account_required","An account of another domain counted as local");++cases;}
  {fakenet::reset();fakenet::use=SidTypeGroup;require(refused(L"fixture")=="local_account_required","A group counted as a user account");++cases;}
  {fakenet::reset();fakenet::linked=TRUE;require(refused(L"fixture")=="local_account_required","A Microsoft account counted as local");++cases;}
  struct Fake{DWORD blank=ERROR_ACCOUNT_RESTRICTION,other=ERROR_LOGON_FAILURE,setResult=NERR_Success;std::wstring sidAfter,nameAfter,domainAfter,osPassword;bool throwOnSet=false,throwOnLookup=false,concurrentCreation=false,unusableAfter=false;int lookups=0;std::vector<std::wstring> logons,sets;};
  const auto access=[&](Fake& f){
    account_password::Access a;
    a.account=[&f](const std::wstring& name){if(f.throwOnLookup)throw std::runtime_error("PRIVATE_SENTINEL-Pw1!");auto account=autologon::localAccount(name,fakenet::lookup);if(++f.lookups>1){if(!f.sidAfter.empty())account.sid=f.sidAfter;if(!f.nameAfter.empty())account.name=f.nameAfter;if(!f.domainAfter.empty())account.domain=f.domainAfter;if(f.unusableAfter)account.usable=false;if(f.concurrentCreation)f.osPassword=L"EXTERNAL-FIXTURE";}return account;};
    a.logon=[&f](const autologon::Account&,const autologon::Secret& password){f.logons.push_back(password.text);return password.text.empty()?f.blank:f.other;};
    a.set=[&f](const autologon::Account& account,const autologon::Secret& password)->DWORD{
      if(f.throwOnSet)throw std::runtime_error("PRIVATE_SENTINEL-Pw1!");f.sets.push_back(account.name+L"|"+password.text);
      fakenet::onChange=[&f](LPCWSTR,LPCWSTR,LPCWSTR oldValue,LPCWSTR newValue)->NET_API_STATUS{
        if(!oldValue||f.osPassword!=oldValue)return ERROR_INVALID_PASSWORD;
        if(f.setResult==NERR_Success)f.osPassword=newValue;return f.setResult;};
      return account_password::changeLocalFromEmpty(account,password,fakenet::lookup,fakenet::change);
    };
    a.minimumLength=[]{return 8u;};a.random=[]{return randomName()+L"#Aa1";};return a;};
  const auto quote=[](const std::string& text){return jsonQuote(text);};
  const auto ask=[&](const std::string& username,const std::string& sid,const std::string& password,const std::string& confirmation,bool confirmed=true,const std::string& extra=""){
    return "{\"action\":\"setWindowsAccountPassword\",\"username\":"+quote(username)+",\"sid\":"+quote(sid)+",\"password\":"+quote(password)+",\"passwordConfirmation\":"+quote(confirmation)+",\"confirmed\":"+(confirmed?"true":"false")+extra+'}';};
  const std::string sid=utf8(fixtureSid);
  std::vector<std::string> responses;
  const auto set=[&](Fake& f,const std::string& request){fakenet::reset();const auto text=account_password::setPasswordResult(access(f),JsonParser::parse(request));responses.push_back(text);return JsonParser::parse(text).get("error").string();};
  // Empty password (the blank-password restriction, then a random one refused as wrong): set once.
  {Fake f;require(set(f,ask("fixture",sid,secret,secret)).empty()&&f.sets==std::vector<std::wstring>{L"fixture|"+wide(secret)},"An empty password was not filled");
   require(f.logons.size()==2&&f.logons[0].empty()&&!f.logons[1].empty()&&f.logons[1]!=wide(secret),"The empty-password check was not the blank then a random password");++cases;}
  {Fake f;f.blank=ERROR_SUCCESS;require(set(f,ask(".\\fixture",sid,secret,secret)).empty()&&f.sets.size()==1&&f.logons.size()==1,"An empty password Windows accepted was not filled");++cases;}
  {Fake f;require(set(f,ask("fixture",sid,secret,secret)).empty()&&fakenet::changes==1&&fakenet::changeServer==L"\\\\FIXTURE-PC"&&fakenet::changeName==L"fixture"&&fakenet::oldPassword.empty()&&fakenet::newPassword==wide(secret),"The production adapter did not bind an explicit local server and empty old password");++cases;}
  // SAME SID: another actor creates a password after the empty-state read.
  // The injected OS primitive compares OLD password atomically; no reset fallback.
  {Fake f;f.concurrentCreation=true;require(set(f,ask("fixture",sid,secret,secret))=="password_exists"&&f.osPassword==L"EXTERNAL-FIXTURE"&&fakenet::changes==1&&f.sets.size()==1,"A concurrently created same-SID password was reset or reported created");++cases;}
  for(const DWORD code:{DWORD(ERROR_INVALID_PASSWORD),DWORD(ERROR_WRONG_PASSWORD),DWORD(ERROR_LOGON_FAILURE)}){Fake f;f.setResult=code;require(set(f,ask("fixture",sid,secret,secret))=="password_exists"&&f.osPassword.empty()&&fakenet::changes==1,"Invalid old password was reported created or retried");++cases;}
  // An existing password is never reset.
  for(const DWORD code:{DWORD(ERROR_LOGON_FAILURE),DWORD(ERROR_WRONG_PASSWORD)}){Fake f;f.blank=code;require(set(f,ask("fixture",sid,secret,secret))=="password_exists"&&f.sets.empty(),"An existing password was reset");++cases;}
  {Fake f;f.other=ERROR_ACCOUNT_RESTRICTION;require(set(f,ask("fixture",sid,secret,secret))=="password_state_unknown"&&f.sets.empty(),"A restriction before the password check counted as an empty password");++cases;}
  for(const DWORD code:{DWORD(ERROR_LOGON_TYPE_NOT_GRANTED),DWORD(ERROR_ACCOUNT_LOCKED_OUT),DWORD(ERROR_INVALID_LOGON_HOURS)}){Fake f;f.blank=code;require(set(f,ask("fixture",sid,secret,secret))=="password_state_unknown"&&f.sets.empty(),"An unproven empty password was filled");++cases;}
  // Only the exact local account the state check showed.
  {Fake f;require(set(f,ask("CORP\\fixture",sid,secret,secret))=="local_account_required"&&f.logons.empty()&&f.sets.empty(),"A domain account got a password");++cases;}
  {Fake f;fakenet::reset();fakenet::linked=TRUE;const auto text=account_password::setPasswordResult(access(f),JsonParser::parse(ask("fixture",sid,secret,secret)));responses.push_back(text);
   require(JsonParser::parse(text).get("error").string()=="local_account_required"&&f.logons.empty()&&f.sets.empty(),"A Microsoft account got a password");++cases;}
  {Fake f;require(set(f,ask("fixture","S-1-5-21-1000-2000-3000-1002",secret,secret))=="account_changed"&&f.logons.empty()&&f.sets.empty(),"Another account's SID was accepted");++cases;}
  {Fake f;require(set(f,ask("fixture","",secret,secret))=="account_changed"&&f.sets.empty(),"A request without the SID was accepted");++cases;}
  {Fake f;f.sidAfter=L"S-1-5-21-1000-2000-3000-1002";require(set(f,ask("fixture",sid,secret,secret))=="account_changed"&&f.sets.empty(),"An account replaced before the change got the password");++cases;}
  {Fake f;f.nameAfter=L"replacement";require(set(f,ask("fixture",sid,secret,secret))=="account_changed"&&f.sets.empty(),"A changed account name passed the final guard");++cases;}
  {Fake f;f.domainAfter=L"CORP";require(set(f,ask("fixture",sid,secret,secret))=="account_changed"&&f.sets.empty(),"A changed account domain passed the final guard");++cases;}
  {Fake f;f.unusableAfter=true;require(set(f,ask("fixture",sid,secret,secret))=="account_restricted"&&f.sets.empty(),"A newly restricted account passed the final guard");++cases;}
  // Exercise production primitive routing directly, with synthetic local lookup.
  // No real Windows mutation, credentials or account lookup is used.
  {fakenet::reset();auto account=autologon::localAccount(L"fixture",fakenet::lookup);account.domain=L"CORP";require(account_password::changeLocalFromEmpty(account,autologon::Secret{wide(secret)},fakenet::lookup,fakenet::change)==ERROR_INVALID_PARAMETER&&fakenet::changes==0,"A domain target reached the mutation");++cases;}
  {fakenet::reset();auto account=autologon::localAccount(L"fixture",fakenet::lookup);account.sid=L"S-1-5-21-1000-2000-3000-1002";require(account_password::changeLocalFromEmpty(account,autologon::Secret{wide(secret)},fakenet::lookup,fakenet::change)==NERR_UserNotFound&&fakenet::changes==0,"The adapter ignored a changed SID");++cases;}
  {fakenet::reset();auto account=autologon::localAccount(L"fixture",fakenet::lookup);fakenet::machineName=L"\\\\REMOTE";require(account_password::changeLocalFromEmpty(account,autologon::Secret{wide(secret)},fakenet::lookup,fakenet::change)==ERROR_INVALID_PARAMETER&&fakenet::changes==0,"A path-shaped machine target reached the mutation");++cases;}
  {fakenet::reset();auto account=autologon::localAccount(L"fixture",fakenet::lookup);fakenet::flags|=UF_ACCOUNTDISABLE;require(account_password::changeLocalFromEmpty(account,autologon::Secret{wide(secret)},fakenet::lookup,fakenet::change)==NERR_UserNotFound&&fakenet::changes==0,"A disabled account reached the mutation");++cases;}
  {Fake f;fakenet::reset();fakenet::flags|=UF_ACCOUNTDISABLE;const auto text=account_password::setPasswordResult(access(f),JsonParser::parse(ask("fixture",sid,secret,secret)));responses.push_back(text);
   require(JsonParser::parse(text).get("error").string()=="account_restricted"&&f.logons.empty()&&f.sets.empty(),"A disabled account got a password");++cases;}
  // The request itself: one confirmation, equal entries, a usable text, no other fields.
  {Fake f;require(set(f,ask("fixture",sid,secret,secret,false))=="confirmation_required"&&!f.lookups&&f.sets.empty(),"An unconfirmed password change ran");++cases;}
  {Fake f;require(set(f,ask("fixture",sid,secret,secret+"x"))=="password_confirmation"&&!f.lookups,"Different entries were accepted");++cases;}
  for(const auto& bad:{std::string(),std::string("a\tb"),std::string(257,'a')}){Fake f;require(set(f,ask("fixture",sid,bad,bad))=="password_invalid_input"&&!f.lookups&&f.sets.empty(),"An unusable password text was set");++cases;}
  {Fake f;require(set(f,ask("fixture",sid,secret,secret,true,",\"takeOwnership\":true"))=="invalid_request"&&!f.lookups,"An unknown field was accepted");++cases;}
  // Windows' password policy is a fixed code; nothing else is passed on.
  for(const DWORD code:{DWORD(NERR_PasswordTooShort),DWORD(ERROR_PASSWORD_RESTRICTION),DWORD(NERR_PasswordNotComplexEnough),DWORD(NERR_PasswordHistConflict),DWORD(NERR_PasswordTooLong)}){Fake f;f.setResult=code;require(set(f,ask("fixture",sid,secret,secret))=="password_policy"&&f.sets.size()==1,"A policy refusal was not reported as policy");++cases;}
  {Fake f;f.setResult=ERROR_ACCESS_DENIED;require(set(f,ask("fixture",sid,secret,secret))=="password_set_denied","Access denied changed meaning");++cases;}
  {Fake f;f.setResult=ERROR_INVALID_PARAMETER;require(set(f,ask("fixture",sid,secret,secret))=="password_set_failed","An unknown failure was reported as done");++cases;}
  {Fake f;f.throwOnSet=true;require(set(f,ask("fixture",sid,secret,secret))=="password_set_failed","A thrown failure was not a fixed code");++cases;}
  {Fake f;f.throwOnLookup=true;require(set(f,ask("fixture",sid,secret,secret))=="password_set_failed"&&f.sets.empty(),"A thrown lookup failure was not a fixed code");++cases;}
  // windowsAccountPasswordState: the account's SID, whether it has a password, the minimum length.
  const auto state=[&](Fake& f,const char* username){fakenet::reset();const auto text=account_password::passwordStateResult(access(f),JsonParser::parse(std::string("{\"action\":\"windowsAccountPasswordState\",\"username\":")+jsonQuote(username)+"}"));responses.push_back(text);return JsonParser::parse(text);};
  {Fake f;const auto v=state(f,"fixture");require(v.get("error").string().empty()&&v.get("state").string()=="empty"&&v.get("sid").string()==sid&&v.get("minimumLength").number()==8&&f.sets.empty(),"An empty password was not reported with its account");++cases;}
  {Fake f;f.blank=ERROR_LOGON_FAILURE;require(state(f,"fixture").get("state").string()=="set"&&f.logons.size()==1,"An existing password was not reported");++cases;}
  {Fake f;f.other=ERROR_ACCOUNT_RESTRICTION;require(state(f,"fixture").get("state").string()=="unknown","An unproven empty password was reported");++cases;}
  {Fake f;require(state(f,"CORP\\fixture").get("error").string()=="local_account_required"&&f.logons.empty(),"A domain account was checked");++cases;}
  {Fake f;f.throwOnLookup=true;require(state(f,"fixture").get("error").string()=="account_unavailable","A thrown lookup failure was not a fixed code");++cases;}
  // No response carries the password or exception text.
  for(const auto& text:responses)require(text.find("PRIVATE_SENTINEL")==std::string::npos&&text.find("Pw1")==std::string::npos,"A password or exception text reached a response");
  ++cases;
  return cases;
}
struct FixtureGuard final:ConnectionGuard{};
DisplayHelperOutput idleFixture(){DisplayHelperOutput result;result.exitCode=0;result.value=JsonParser::parse(R"({"readOnly":true,"modeCatalogRevision":1,"requiredModeCount":18,"modeCatalogReady":false,"configurationPending":true,"reason":"mode_catalog_update_required","error":"","recoveryRequired":false,"state":{"recoveryRequired":false}})");return result;}
void credentialRefreshAcknowledgement(){
  for(int scenario=0;scenario<6;++scenario){
    int writes=0,notifications=0;bool saved=false,failed=false;std::string response;
    try{response=settingsApplyWithCapabilitiesRefresh([&]{
      if(scenario==3)throw WindowsAccountError("rdp_credentials_storage_failed");
      if(scenario==4)saveWindowsAccount("user","password","mismatch",[](const auto&){return DWORD(ERROR_SUCCESS);},[&](const auto&){++writes;});
      if(scenario==5)saveWindowsAccount("user","password","password",[](const auto&){return DWORD(ERROR_LOGON_FAILURE);},[&](const auto&){++writes;});
      ++writes;saved=true;
    },[&]{++notifications;require(saved&&writes==1,"Credential refresh preceded successful persistence");if(scenario==2)throw std::runtime_error("private SCM failure");return scenario!=1;});}
    catch(const WindowsAccountError&){failed=true;}
    if(scenario>=3)require(failed&&!saved&&!writes&&!notifications&&response.empty(),"Failed credential validation/storage sent a refresh or acknowledged success");
    else {const auto value=JsonParser::parse(response);require(!failed&&saved&&writes==1&&notifications==1&&value.get("ok").boolean()&&value.get("capabilitiesRefreshRequested").boolean()==(scenario==0)&&std::get<Json::Object>(value.value).size()==2&&response.find("private")==std::string::npos,"Committed credential change was mislabeled failed/ready or lost separate refresh acknowledgement");}
  }
}
void displayIdleGuards(){
  for(int scenario=0;scenario<12;++scenario){
    auto probe=idleFixture();auto& value=std::get<Json::Object>(probe.value.value);int probes=0,configured=0,reloaded=0;std::vector<int> order;
    if(scenario==2)probe.timedOut=true;
    if(scenario==3)probe.exitCode=1;
    if(scenario==4)value["recoveryRequired"].value=true;
    if(scenario==5)std::get<Json::Object>(value["state"].value)["recoveryRequired"].value=true;
    if(scenario==6)value["reason"].value=std::string("viewport_busy");
    if(scenario==7)value["error"].value=std::string("private helper error must not escape");
    if(scenario==8)value["readOnly"].value=false;
    if(scenario==9)value["reason"].value=std::string("unknown_future_reason");
    if(scenario==10)value["modeCatalogRevision"].value=2.0;
    bool failed=false;std::string error;
    try{configureIdleDisplay({[&]()->ConnectionGuardPtr{order.push_back(1);return scenario==1?ConnectionGuardPtr{}:std::make_unique<FixtureGuard>();},[&]{++probes;order.push_back(2);return probe;},[&]{++configured;order.push_back(3);if(scenario==11)throw std::runtime_error("fixture_configuration_failed");},[&]{++reloaded;order.push_back(4);}});}catch(const std::exception& failure){failed=true;error=failure.what();}
    require(error.find("private")==std::string::npos,"Untrusted helper error escaped display RPC");
    if(!scenario)require(!failed&&probes==1&&configured==1&&reloaded==1&&order==std::vector<int>({1,2,3,4}),"Idle configuration did not keep probe/configure/reload ordering");
    else require(failed&&reloaded==0&&configured==(scenario==11?1:0)&&probes==(scenario==1?0:1),"Busy/unknown/recovery display change configured or reloaded the service");
  }
}
// devices-v1: fixed codes only; unread or unexpected values stay "unknown" and
// never carry device names, paths or error text into the settings window.
int deviceStatusVocabulary(){
  int cases=0;
  DeviceObservation unread;auto value=JsonParser::parse(deviceStatusJson(unread));
  require(value.get("schema").number()==1&&!value.get("drives").get("winfsp").boolean(true)&&value.get("drives").get("webClient").string()=="unknown"&&!value.get("usb").get("supported").boolean(true)&&!value.get("usb").get("package").boolean(true)&&value.get("usb").get("controller").string()=="unknown"&&value.get("printers").get("reason").string()=="unknown"&&value.get("audio").get("output").string()=="unknown","Unread device state was reported as a known state");++cases;
  DeviceObservation ready;ready.winfsp=true;ready.webClient="usable";ready.usbPlatform=ready.usbPackage=true;ready.usbController="started";ready.print="";ready.audio="present";value=JsonParser::parse(deviceStatusJson(ready));
  require(value.get("drives").get("winfsp").boolean()&&value.get("drives").get("webClient").string()=="usable"&&value.get("usb").get("supported").boolean()&&value.get("usb").get("package").boolean()&&value.get("usb").get("controller").string()=="started"&&value.get("printers").get("reason").string().empty()&&value.get("audio").get("output").string()=="present","Observed device state was lost");++cases;
  for(int owner=0;owner<=7;++owner){const char* name=usb::controllerOwnerName(usb::ControllerOwner(owner));DeviceObservation item;item.usbController=name;require(JsonParser::parse(deviceStatusJson(item)).get("usb").get("controller").string()==name,"USB controller owner changed meaning");++cases;}
  for(const char* reason:{"print_spooler_unavailable","print_driver_unavailable"}){DeviceObservation item;item.print=reason;require(JsonParser::parse(deviceStatusJson(item)).get("printers").get("reason").string()==reason,"Printer reason changed");++cases;}
  for(const char* output:{"absent","remote"}){DeviceObservation item;item.audio=output;require(JsonParser::parse(deviceStatusJson(item)).get("audio").get("output").string()==output,"Audio state changed");++cases;}
  for(const char* spooler:{"running","stopped","starting","disabled","missing"}){DeviceObservation item;item.spooler=spooler;require(JsonParser::parse(deviceStatusJson(item)).get("printers").get("spooler").string()==spooler,"Spooler state changed");++cases;}
  value=JsonParser::parse(deviceStatusJson(unread));require(!value.get("drives").get("package").boolean(true)&&value.get("printers").get("spooler").string()=="unknown","Unread WinFsp package or spooler was reported");
  {DeviceObservation item;item.winfspPackage=true;require(JsonParser::parse(deviceStatusJson(item)).get("drives").get("package").boolean(),"Carried WinFsp package was lost");++cases;}
  DeviceObservation hostile;hostile.webClient="C:\\PRIVATE_SENTINEL";hostile.print="PRIVATE_SENTINEL error";hostile.audio="Speakers (PRIVATE_SENTINEL)";hostile.spooler="PRIVATE_SENTINEL";hostile.usbController="PRIVATE_SENTINEL";
  const auto text=deviceStatusJson(hostile);value=JsonParser::parse(text);
  require(text.find("PRIVATE_SENTINEL")==std::string::npos&&value.get("drives").get("webClient").string()=="unknown"&&value.get("printers").get("reason").string()=="unknown"&&value.get("printers").get("spooler").string()=="unknown"&&value.get("audio").get("output").string()=="unknown"&&value.get("usb").get("controller").string()=="unknown","Unexpected device text escaped the fixed vocabulary");++cases;
  // This machine, read-only: whatever it is, it answers in the vocabulary.
  value=JsonParser::parse(observeDevices());
  const auto in=[](const std::string& item,std::initializer_list<const char*> allowed){for(const char* a:allowed)if(item==a)return true;return false;};
  require(in(value.get("drives").get("webClient").string(),{"usable","disabled","missing","unknown"})&&in(value.get("usb").get("controller").string(),{"none","started","stopped","unmarked","duplicates","foreign","multiple","unknown"})&&in(value.get("printers").get("reason").string(),{"","print_spooler_unavailable","print_driver_unavailable","unknown"})&&in(value.get("printers").get("spooler").string(),{"running","stopped","starting","disabled","missing","unknown"})&&in(value.get("audio").get("output").string(),{"present","absent","remote","unknown"}),"Live device observation left the vocabulary");++cases;
  return cases;
}
// installWinfsp: the setup's own WinFsp step, only confirmed and only while
// missing; restart is reported, never performed (a Fake backend: nothing runs).
int winfspInstallFromSettings(){
  int cases=0;
  std::unique_ptr<multitap::winfsp::Fake> backend;
  const auto run=[&](const char* mode,bool installed,bool confirmed){backend=std::make_unique<multitap::winfsp::Fake>(mode);return JsonParser::parse(winfspInstallResult(*backend,L"pinned.msi",installed,confirmed));};
  auto value=run("",false,false);require(value.get("error").string()=="confirmation_required"&&!backend->installs&&!backend->hashes,"Unconfirmed WinFsp install ran");++cases;
  value=run("",true,true);require(value.get("error").string()=="winfsp_already_installed"&&!backend->installs&&!backend->hashes,"Installed WinFsp was installed again");++cases;
  value=run("",false,true);require(value.get("error").string().empty()&&value.get("action").string()=="installed"&&!value.get("restart").boolean(true)&&backend->installs==1&&backend->owned==1,"Missing WinFsp was not installed and recorded");++cases;
  value=run("reboot",false,true);require(value.get("error").string().empty()&&value.get("restart").boolean(),"MSI restart request was lost");++cases;
  value=run("reboot-verify-fails",false,true);require(value.get("error").string()=="winfsp_load_failed"&&value.get("restart").boolean(),"Failed check after a restart request lost either fact");++cases;
  for(const auto& [mode,code]:std::initializer_list<std::pair<const char*,const char*>>{{"install-fails","winfsp_install_failed"},{"busy","winfsp_busy"},{"package-bad","winfsp_package_mismatch"}}){value=run(mode,false,true);require(value.get("error").string()==code&&!value.get("restart").boolean(true),"WinFsp failure code changed");++cases;}
  require(backend->installs==0,"A package that failed its check was installed");
  // The follow-up publication is requested only after success and never relabels it.
  int refreshes=0;const auto refresh=[&]{++refreshes;return true;};
  require(JsonParser::parse(deviceChangeRpc("winfsp",R"({"action":"installed","restart":false,"error":""})",refresh)).get("capabilitiesRefreshRequested").boolean()&&refreshes==1,"Successful install did not request fresh capabilities");++cases;
  require(!JsonParser::parse(deviceChangeRpc("winfsp",R"({"action":"skipped","restart":false,"error":"winfsp_busy"})",refresh)).get("capabilitiesRefreshRequested").boolean(true)&&refreshes==1,"Failed install requested a capabilities refresh");++cases;
  const auto thrown=JsonParser::parse(deviceChangeRpc("spooler",R"({"state":"running","error":""})",[]()->bool{throw std::runtime_error("private SCM failure");}));
  require(thrown.get("ok").boolean()&&thrown.get("spooler").get("state").string()=="running"&&!thrown.get("capabilitiesRefreshRequested").boolean(true),"A failed refresh relabeled the completed change");++cases;
  return cases;
}
// startPrintSpooler: a stopped spooler starts once; Disabled is never touched.
int spoolerStartFromSettings(){
  int cases=0;
  struct Fake{std::vector<std::string> states;DWORD startResult=0;size_t at=0;int starts=0,waits=0;std::string state(){const auto value=states[std::min(at,states.size()-1)];++at;return value;}};
  const auto run=[](Fake& fake){return JsonParser::parse(startPrintSpoolerResult({[&]{return fake.state();},[&]{++fake.starts;return fake.startResult;},[&]{++fake.waits;}}));};
  {Fake fake{{"running"}};const auto v=run(fake);require(v.get("state").string()=="running"&&v.get("error").string().empty()&&!fake.starts,"Running spooler was started again");++cases;}
  {Fake fake{{"disabled"}};const auto v=run(fake);require(v.get("error").string()=="print_spooler_disabled"&&!fake.starts,"Disabled spooler was started");++cases;}
  for(const char* state:{"missing","unknown","starting"}){Fake fake{{state}};const auto v=run(fake);require(v.get("error").string()=="print_spooler_unavailable"&&!fake.starts,"Unstartable spooler state was started");++cases;}
  {Fake fake{{"stopped","starting","starting","running"}};const auto v=run(fake);require(v.get("state").string()=="running"&&v.get("error").string().empty()&&fake.starts==1&&fake.waits==2,"Started spooler was not observed running");++cases;}
  {Fake fake{{"stopped"},ERROR_SERVICE_DISABLED};const auto v=run(fake);require(v.get("error").string()=="print_spooler_disabled"&&fake.starts==1,"Spooler disabled meanwhile was mislabeled");++cases;}
  {Fake fake{{"stopped"},ERROR_ACCESS_DENIED};const auto v=run(fake);require(v.get("error").string()=="print_spooler_start_failed"&&v.get("state").string()=="stopped","Refused start was reported as started");++cases;}
  {Fake fake{{"stopped"}};const auto v=run(fake);require(v.get("error").string()=="print_spooler_start_failed"&&fake.starts==1&&fake.waits==40,"A spooler that never ran was reported running or waited without bound");++cases;}
  return cases;
}
int serviceRestartEnvelopes(){
  int calls=0,cases=0;const auto valid=JsonParser::parse(R"({"action":"restartServiceV1","protocol":"service-restart-v1","confirmed":true})");
  const auto complete=[&]{++calls;service_restart::Result value;value.outcome=service_restart::Outcome::completed;value.stage=service_restart::Stage::completed;value.error=0;value.stopRequested=value.startRequested=value.observedRunning=value.oldExitRequired=value.exactOldExit=true;return value;};
  for(const auto* invalid:{R"({"action":"service","protocol":"service-restart-v1","confirmed":true})",
      R"({"action":"restartServiceV1","protocol":"other","confirmed":true})",
      R"({"action":"restartServiceV1","protocol":"service-restart-v1","confirmed":false})",
      R"({"action":"restartServiceV1","protocol":"service-restart-v1","confirmed":"true"})",
      R"({"action":"restartServiceV1","protocol":"service-restart-v1"})",
      R"({"action":"restartServiceV1","protocol":"service-restart-v1","confirmed":true,"other":1})"}){
    const auto result=JsonParser::parse(serviceRestartRpc(JsonParser::parse(invalid),complete));
    require(!result.get("ok").boolean()&&result.get("error").string()=="confirmation_required"&&!calls,"Unconfirmed/malformed restart invoked native mutation");++cases;
  }
  {const auto result=JsonParser::parse(serviceRestartRpc(valid,complete));
    require(calls==1&&result.get("ok").boolean()&&result.get("restart").get("protocol").string()=="service-restart-v1"&&result.get("restart").get("exactOldExit").boolean(),"Completed native restart proof was not preserved");++cases;}
  {const auto result=JsonParser::parse(serviceRestartRpc(valid,[]{service_restart::Result value;value.outcome=service_restart::Outcome::uncertain;value.stage=service_restart::Stage::waitRunning;value.error=ERROR_TIMEOUT;value.firstError=ERROR_ACCESS_DENIED;value.queryErrors=31;value.stopRequested=value.startRequested=value.oldExitRequired=value.exactOldExit=true;return value;}));
    require(!result.get("ok").boolean()&&result.get("error").string()=="host_service_restart_unverified"&&result.get("restart").get("queryErrors").number()==31&&!result.get("restart").get("observedRunning").boolean(),"Uncertain restart was advertised as complete or lost its outcome");++cases;}
  {const auto result=serviceRestartRpc(valid,[]()->service_restart::Result{throw std::runtime_error("PRIVATE_SENTINEL");});
    require(result.find("PRIVATE_SENTINEL")==std::string::npos&&!JsonParser::parse(result).get("ok").boolean(),"Native restart failure leaked arbitrary exception text or became success");++cases;}
  {require(!JsonParser::parse(serviceRestartRpc(valid,{})).get("ok").boolean(),"Missing native restart backend became success");++cases;}
  return cases;
}
// Who may change an installed program path (support.hpp). Security descriptors
// built in memory from the lists two real installs have; no file is touched.
//  - a folder Windows Installer created under Program Files (a first install
//    from the Host MSI) inherits TrustedInstaller's full control;
//  - the earlier installer gave Multitap\Host its own protected list.
// trustedMachineFile refused the first, so Host settings could neither read the
// broker state ("서버 연결 확인 필요" on a connected Host) nor restart the service.
int installedPathTrust(){
  int cases=0;
  const auto trusted=[](const wchar_t* sddl,bool installer){
    PSECURITY_DESCRIPTOR descriptor=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"QA security descriptor was not built");
    PSID owner=nullptr;PACL dacl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    GetSecurityDescriptorOwner(descriptor,&owner,&defaulted);GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted);
    const bool result=trustedSecurity(owner,present?dacl:nullptr,installer);LocalFree(descriptor);return result;
  };
  constexpr const wchar_t* msiFolder=L"O:SYG:SYD:AI(A;ID;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;CIIOID;GA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;ID;FA;;;SY)(A;OICIIOID;GA;;;SY)(A;ID;FA;;;BA)(A;OICIIOID;GA;;;BA)(A;ID;0x1200a9;;;BU)(A;OICIIOID;GXGR;;;BU)(A;OICIIOID;GA;;;CO)(A;ID;0x1200a9;;;AC)(A;OICIIOID;GXGR;;;AC)(A;ID;0x1200a9;;;S-1-15-2-2)(A;OICIIOID;GXGR;;;S-1-15-2-2)";
  constexpr const wchar_t* msiFile=L"O:SYG:SYD:AI(A;ID;FA;;;SY)(A;ID;FA;;;BA)(A;ID;0x1200a9;;;BU)(A;ID;0x1200a9;;;AC)(A;ID;0x1200a9;;;S-1-15-2-2)";
  constexpr const wchar_t* protectedFolder=L"O:BAG:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
  require(!trustedSecurity(nullptr,nullptr,true),"A path without an owner or list was trusted");++cases;
  require(trusted(msiFolder,true)&&!trusted(msiFolder,false),"A folder of a Windows Installer install must be an installed path, and only that");++cases;
  require(trusted(msiFile,true)&&trusted(msiFile,false),"A file of a Windows Installer install was refused");++cases;
  require(trusted(protectedFolder,true)&&trusted(protectedFolder,false),"The earlier installer's protected folder was refused");++cases;
  // Anyone else with a write right, in any form, is still refused in both modes.
  for(const auto* open:{L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)",L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;AU)",L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;WD)",
                        L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)(A;;0x100;;;BU)",L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;CO)",L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;S-1-5-80-1)"}){
    require(!trusted(open,true)&&!trusted(open,false),"A path someone else can change was trusted");++cases;
  }
  // The owner decides too: a user's own folder is never an installed path.
  require(!trusted(L"O:BUG:BUD:(A;;FA;;;SY)(A;;FA;;;BA)",true)&&!trusted(L"O:S-1-5-21-1-2-3-1001G:SYD:(A;;FA;;;SY)",true),"A user-owned path was trusted");++cases;
  require(trusted(L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SYD:(A;;FA;;;SY)(A;;0x1200a9;;;BU)",true)&&!trusted(L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SYD:(A;;FA;;;SY)",false),"TrustedInstaller as owner belongs to installed paths only");++cases;
  // hostAction 276 stage 2 numbers are fixed.
  require(winfspErrorNumber("")==0&&winfspErrorNumber("winfsp_already_installed")==2&&winfspErrorNumber("winfsp_driver_missing")==10&&winfspErrorNumber("x")==99,"WinFsp diagnostic numbers moved");++cases;
  // Only a finish step writes the journal from the settings process, and only while it runs.
  require(!operationalLogEnabled,"QA journal was enabled before the scope");{FinishJournal journal;require(operationalLogEnabled,"The finish scope did not enable the journal");}
  require(!operationalLogEnabled,"The finish scope left the journal enabled");++cases;
  return cases;
}
// The installed tool the service starts at boot to repair an interrupted
// automatic-login change (windows_autologon_hooks.hpp). The same in-memory
// lists stand in for the folders and files of an install; nothing is read from
// or written to Program Files. With the earlier check (SYSTEM and Administrators
// only) every one of these installs was refused, so the repair never ran.
int installedRecoveryToolLookup(){
  int cases=0;
  constexpr const wchar_t* msiFolder=L"O:SYG:SYD:AI(A;ID;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;CIIOID;GA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;ID;FA;;;SY)(A;OICIIOID;GA;;;SY)(A;ID;FA;;;BA)(A;OICIIOID;GA;;;BA)(A;ID;0x1200a9;;;BU)(A;OICIIOID;GXGR;;;BU)(A;OICIIOID;GA;;;CO)(A;ID;0x1200a9;;;AC)(A;OICIIOID;GXGR;;;AC)(A;ID;0x1200a9;;;S-1-15-2-2)(A;OICIIOID;GXGR;;;S-1-15-2-2)";
  constexpr const wchar_t* msiFile=L"O:SYG:SYD:AI(A;ID;FA;;;SY)(A;ID;FA;;;BA)(A;ID;0x1200a9;;;BU)(A;ID;0x1200a9;;;AC)(A;ID;0x1200a9;;;S-1-15-2-2)";
  constexpr const wchar_t* protectedFolder=L"O:BAG:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
  constexpr const wchar_t* protectedFile=L"O:BAG:BAD:AI(A;ID;FA;;;SY)(A;ID;FA;;;BA)(A;ID;0x1200a9;;;BU)";
  constexpr const wchar_t* userFolder=L"O:SYG:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)";
  const std::filesystem::path programFiles=L"C:\\Program Files",product=programFiles/L"Multitap",root=product/L"Host",bin=root/L"bin";
  const auto own=bin/L"MultitapHost.exe",tool=bin/L"MultitapHostTool.exe",marker=root/L"multitap-host.json";
  struct Install{std::map<std::wstring,const wchar_t*> lists;std::string marker;};
  const auto files=[](const Install& install,bool installer){
    InstalledHostFiles out;
    out.trusted=[&install,installer](const std::filesystem::path& path){
      const auto found=install.lists.find(path.wstring());if(found==install.lists.end())return false;  // not there
      PSECURITY_DESCRIPTOR descriptor=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(found->second,SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"QA security descriptor was not built");
      PSID owner=nullptr;PACL dacl=nullptr;BOOL present=FALSE,defaulted=FALSE;
      GetSecurityDescriptorOwner(descriptor,&owner,&defaulted);GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted);
      const bool result=trustedSecurity(owner,present?dacl:nullptr,installer);LocalFree(descriptor);return result;
    };
    out.read=[&install](const std::filesystem::path&){return install.marker;};
    return out;
  };
  const std::string version="1.0.53",good=R"({"product":"MultitapHost","version":"1.0.53","service":"MultitapHost"})";
  const auto find=[&](const Install& install,bool installer=true,std::filesystem::path image={}){return installedHostTool(programFiles,image.empty()?own:image,version,files(install,installer));};
  // A first install from the Host MSI: every folder inherits Program Files' list.
  Install msi{{{product.wstring(),msiFolder},{root.wstring(),msiFolder},{bin.wstring(),msiFolder},{marker.wstring(),msiFile},{own.wstring(),msiFile},{tool.wstring(),msiFile}},good};
  require(find(msi)==tool,"The recovery tool of a Windows Installer install was not found");++cases;
  require(find(msi,false).empty(),"The earlier check would have found it: the fixture no longer shows the defect");++cases;
  // An install that began with the earlier installer: Host has its own
  // protected list, but Program Files\Multitap is still a Windows Installer folder.
  Install upgraded{{{product.wstring(),msiFolder},{root.wstring(),protectedFolder},{bin.wstring(),protectedFolder},{marker.wstring(),protectedFile},{own.wstring(),protectedFile},{tool.wstring(),protectedFile}},good};
  require(find(upgraded)==tool&&find(upgraded,false).empty(),"An install upgraded from the earlier installer must be found, and was refused before");++cases;
  // A byte-order mark in front of the marker is accepted, as before.
  {auto marked=msi;marked.marker="\xef\xbb\xbf"+good;require(find(marked)==tool,"A marker with a byte-order mark was refused");++cases;}
  // Anything a standard user can change is still refused, at every level.
  for(const auto& open:{product,root,bin,marker,own,tool}){auto weak=msi;weak.lists[open.wstring()]=userFolder;require(find(weak).empty(),"An install a user can change was trusted");++cases;}
  // A missing folder or file is refused.
  for(const auto& gone:{product,root,bin,marker,own,tool}){auto partial=msi;partial.lists.erase(gone.wstring());require(find(partial).empty(),"An incomplete install was trusted");++cases;}
  // Only the installed service image may ask: a copy elsewhere, or the tool itself, is refused.
  require(find(msi,true,L"C:\\Users\\qa\\Downloads\\MultitapHost.exe").empty()&&find(msi,true,tool).empty()&&find(msi,true,root/L"MultitapHost.exe").empty(),"A process outside the installed service image was accepted");++cases;
  require(find(msi,true,L"c:\\program files\\multitap\\host\\bin\\..\\bin\\multitaphost.exe")==tool,"The installed image written another way was refused");++cases;
  // The marker must name this product, this service and this exact version.
  for(const auto* wrong:{"",R"({"product":"MultitapHost","version":"1.0.52","service":"MultitapHost"})",R"({"product":"Other","version":"1.0.53","service":"MultitapHost"})",
                         R"({"product":"MultitapHost","version":"1.0.53","service":"Other"})",R"({"product":"MultitapHost","version":"1.0.53")","not json"}){
    auto other=msi;other.marker=wrong;require(find(other).empty(),"A marker of another install was accepted");++cases;
  }
  {auto large=msi;large.marker=good+std::string(8200,' ');require(find(large).empty(),"An oversized marker was accepted");++cases;}
  require(installedHostTool(programFiles,own,version,InstalledHostFiles{}).empty()&&installedHostTool({},own,version,files(msi,true)).empty(),"A lookup without file access or a Program Files folder succeeded");++cases;
  return cases;
}
void realMutexGuard(){  // A unique user-owned QA mutex exercises contention without touching the
  // production Global mutex, service, registry or display helper.
  const std::wstring name=L"Local\\MultitapHost.DisplayMaintenance.QA."+randomName();
  PSECURITY_DESCRIPTOR descriptor=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)",SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"QA mutex ACL creation failed");SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};WinHandle handle(CreateMutexW(&attributes,FALSE,name.c_str()));LocalFree(descriptor);require(handle.value!=nullptr,"QA mutex creation failed");
  const auto acquire=[&]()->ConnectionGuardPtr{const DWORD waited=WaitForSingleObject(handle.value,0);if(waited==WAIT_TIMEOUT)return {};require(waited==WAIT_OBJECT_0||waited==WAIT_ABANDONED,"QA mutex wait failed");HANDLE duplicate=nullptr;require(DuplicateHandle(GetCurrentProcess(),handle.value,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"QA mutex duplicate failed");return std::make_unique<NativeConnectionGuard>(duplicate,true);};
  const auto competitor=[&]{bool busy=false;std::jthread other([&]{busy=!acquire();});other.join();return busy;};
  int checks=0;
  configureIdleDisplay({acquire,[&]{++checks;require(competitor(),"Automatic catalog could enter during read-only RPC probe");return idleFixture();},[&]{++checks;require(competitor(),"Automatic catalog could enter during display configuration");},[&]{++checks;require(competitor(),"Automatic catalog could enter before service reload finished");}});
  require(checks==3&&!competitor(),"Display RPC leaked its mutex after completion");
  auto automatic=acquire();require(bool(automatic),"QA automatic maintenance gate unavailable");bool rejected=false;
  std::jthread rpc([&]{try{configureIdleDisplay({acquire,[]()->DisplayHelperOutput{throw std::runtime_error("Probe ran during busy maintenance");},[]{throw std::runtime_error("Configuration ran during busy maintenance");},[]{throw std::runtime_error("Reload ran during busy maintenance");}});}catch(const std::exception& error){rejected=std::string(error.what())=="viewport_busy";}});rpc.join();require(rejected,"RPC did not fail without reload while automatic catalog held the gate");
  // The production lock function additionally refuses this user-owned ACL.
  bool refused=false;try{const auto invalid=nativeConnectionLock(name.c_str(),0);(void)invalid;}catch(...){refused=true;}require(refused,"Production gate accepted an untrusted pre-created mutex");
}
}
int main(){try{
  require(!operationalLogEnabled,"QA must not write the real host journal");
  const auto safeLog=safeHostLogRecord(JsonParser::parse(R"({"time":"2026-10-23T09:00:00.000Z","event":"displayObserved","values":{"reboot":true,"monitors":2,"password":"PRIVATE_SENTINEL","error":"PRIVATE_SENTINEL"},"token":"PRIVATE_SENTINEL"})"));
  require(safeLog.find("PRIVATE_SENTINEL")==std::string::npos&&safeLog.find("reboot")!=std::string::npos&&safeLog.find("monitors")!=std::string::npos,"Journal leaked arbitrary or secret text");
  require(safeHostLogRecord(JsonParser::parse(R"({"time":"PRIVATE_SENTINEL","event":"displayObserved","values":{}})")).empty(),"Journal accepted invalid timestamp");
  DisplayHelperOutput plan;plan.exitCode=0;plan.value=JsonParser::parse("{\"planId\":\""+std::string(64,'a')+"\",\"canRepair\":true,\"items\":[{\"kind\":\"device\",\"name\":\"QA virtual adapter\"}]}");auto output=JsonParser::parse(displayRepairRpcResponse(plan,true));require(output.get("ok").boolean()&&output.get("plan").get("planId").string()==std::string(64,'a')&&output.get("helperExitCode").number(-1)==0&&!output.get("timedOut").boolean(),"Read-only repair plan envelope lost helper result");
  DisplayHelperOutput partial;partial.exitCode=1;partial.value=JsonParser::parse(R"({"repairInstalled":false,"cleanupCompleted":false,"partialRemoval":true,"removedDeviceCount":1,"reason":"device_remove_failed","rebootRequired":true})");output=JsonParser::parse(displayRepairRpcResponse(partial,false));require(output.get("ok").boolean()&&output.get("helperExitCode").number()==1&&!output.get("result").get("repairInstalled").boolean()&&output.get("result").get("partialRemoval").boolean()&&output.get("result").get("removedDeviceCount").number()==1&&output.get("result").get("rebootRequired").boolean()&&output.get("result").get("reason").string()=="device_remove_failed","Partial repair was hidden or mislabeled successful");
  DisplayHelperOutput timeout;timeout.timedOut=true;timeout.exitCode=ERROR_TIMEOUT;output=JsonParser::parse(displayRepairRpcResponse(timeout,false));require(output.get("timedOut").boolean()&&output.get("helperExitCode").number()==ERROR_TIMEOUT&&std::holds_alternative<std::nullptr_t>(output.get("result").value),"Unknown timed-out result was fabricated");
  credentialRefreshAcknowledgement();displayIdleGuards();realMutexGuard();const int deviceCases=deviceStatusVocabulary(),winfspCases=winfspInstallFromSettings(),spoolerCases=spoolerStartFromSettings(),adapterCases=displayAdapterOwnership()+displayAdapterCleanupRequest(),usbReplaceCases=usbControllerReplacement(),passwordCases=accountPasswordSet(),restartCases=serviceRestartEnvelopes();const int trustCases=installedPathTrust();require(trustCases==14,"Installed path trust cases were skipped");const int recoveryToolCases=installedRecoveryToolLookup();require(recoveryToolCases==26,"Installed recovery tool cases were skipped");
  std::cout<<"{\"kind\":\"host-settings-rpc-result-envelope\",\"fixtureCases\":21,\"deviceStatusCases\":"<<deviceCases<<",\"winfspInstallCases\":"<<winfspCases<<",\"spoolerStartCases\":"<<spoolerCases<<",\"displayAdapterCases\":"<<adapterCases<<",\"usbReplaceCases\":"<<usbReplaceCases<<",\"accountPasswordCases\":"<<passwordCases<<",\"serviceRestartEnvelopeCases\":"<<restartCases<<",\"installedPathTrustCases\":"<<trustCases<<",\"installedRecoveryToolCases\":"<<recoveryToolCases<<",\"windowsPasswordChanged\":false,\"devicesRemoved\":0,\"msiexecRun\":false,\"spoolerStarted\":false,\"credentialRefreshAcknowledgedSeparately\":true,\"committedCredentialsSurviveRefreshFailure\":true,\"realPrivateMutexContention\":true,\"untrustedLockRejected\":true,\"busyRecoveryFailureNoReload\":true,\"partialHelperFailurePreserved\":true,\"timeoutUncertaintyPreserved\":true,\"helperExecuted\":false,\"windowsDeviceOrRegistryChanged\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
