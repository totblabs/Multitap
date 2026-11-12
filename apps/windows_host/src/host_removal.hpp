#pragma once
// host-removal-v1: what removing the Host takes away besides its program files.
//
// The Host is a Windows Installer package: removal (Windows "앱 제거", or
// msiexec /x) deleted the files, the service and the shortcuts and nothing
// else. Left behind were the virtual display device node, its driver package
// in the driver store, the Sunshine driver certificate in LocalMachine Root and
// TrustedPublisher, and the provider selection with its display state.
//
// The package now runs this once, as SYSTEM, after Windows Installer stopped
// and deleted the service and before it removes the files
// (tools/host-installer/simplify-msi.vbs):
//   MultitapHostTool.exe --product-removal-cleanup --remove=[REMOVE]
//       --upgrading=[UPGRADINGPRODUCTCODE] --product=[ProductCode]
//
// An update is a Windows Installer major upgrade: the new package removes the
// old product first. Cleaning up then would take the display driver away from
// a Host that is only being updated, so three independent things must all say
// "real removal", and anything unexpected means "do nothing":
//   1. the package schedules the action only when REMOVE="ALL" and
//      UPGRADINGPRODUCTCODE is not set (Windows Installer sets it in the
//      removal that belongs to an upgrade);
//   2. the same two properties arrive in the command line and are checked
//      again here (an upgrade's product code, or text Windows Installer did not
//      resolve, refuses), and the product code must be the installed one;
//   3. no Multitap Host installer is running (every install and update holds
//      its machine-wide mutex for the whole msiexec run).
//
// What a real removal takes away, and only that:
//   - device nodes Multitap can prove are its own (display_adapter_inventory.hpp:
//     the protected selection, the HKLM ledger, the legacy owner marker). A
//     node that only looks like ours stays;
//   - the driver package (the pinned original bytes) when Multitap had a node
//     and no device at all is bound to it any more; Windows itself refuses to
//     delete a package a device still uses;
//   - the certificate only from the stores Multitap's own install added it to
//     (native_display_trust.hpp) and only when nothing on the machine needs
//     the publisher any more (no device with the driver's hardware ID, no
//     driver package naming it);
//   - the provider selection and the display state records.
// Never touched: WinFsp (a separate product other programs may use), the Host's
// registration and settings, Windows accounts and automatic login, any other
// certificate, any Windows security setting. The signed driver files are only
// deleted with their package, never changed.
//
// It runs only when the Host service is stopped and no saved stream display
// layout waits to be restored; otherwise nothing is removed (the leftovers
// stay as they did before this existed). Removing trust or a device is never
// required for the removal itself: Windows Installer ignores the exit code.
//
// This header holds every decision and is tested on fakes
// (host_removal_test.cpp); host_removal_native.hpp makes the calls.
#include <optional>
#include <string>
#include <vector>

namespace host::removal {

// ---- Is this a real removal? ------------------------------------------------
struct Request {
  bool removal=false;
  const char* refusal="arguments";  // arguments | not_removal | upgrade | product
};
// {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}, upper-case hex as Windows Installer writes it.
inline bool productCode(const std::wstring& value){
  if(value.size()!=38||value.front()!=L'{'||value.back()!=L'}')return false;
  for(size_t index=1;index+1<value.size();++index){
    const wchar_t ch=value[index];
    if(index==9||index==14||index==19||index==24){if(ch!=L'-')return false;}
    else if(!((ch>=L'0'&&ch<=L'9')||(ch>=L'A'&&ch<=L'F')))return false;
  }
  return true;
}
// `arguments`: everything after --product-removal-cleanup. Exactly the three
// values the package writes, in its order. `installed`: the product code of the
// install this program belongs to (its marker file).
inline Request parseRequest(const std::vector<std::wstring>& arguments,const std::wstring& installed){
  Request out;
  const std::wstring remove=L"--remove=",upgrading=L"--upgrading=",product=L"--product=";
  if(arguments.size()!=3||arguments[0].rfind(remove,0)!=0||arguments[1].rfind(upgrading,0)!=0||arguments[2].rfind(product,0)!=0)return out;
  // Anything after "--upgrading=" is an upgrade: the new product's code, or a
  // property name Windows Installer left unresolved.
  if(arguments[1].size()!=upgrading.size()){out.refusal="upgrade";return out;}
  if(arguments[0].substr(remove.size())!=L"ALL"){out.refusal="not_removal";return out;}
  const auto code=arguments[2].substr(product.size());
  if(!productCode(code)||!productCode(installed)||code!=installed){out.refusal="product";return out;}
  out.removal=true;out.refusal="";
  return out;
}

// ---- The machine ------------------------------------------------------------
enum class Presence {absent,present,foreign,unknown};
struct Node {std::wstring instance;bool legacy=false;};
enum class PackageRemoval {removed,inUse,failed};

class Access {
 public:
  virtual ~Access()=default;
  // A Multitap Host installer (install or update) is running on this machine.
  virtual bool installerRunning()=0;
  // installerRunning() saw a lock of that name that is not ours (any user
  // can create Global\ objects first) and ignored it.
  virtual bool foreignLockIgnored(){return false;}
  // The Host service is stopped or no longer registered.
  virtual bool serviceStopped()=0;
  // No stream display layout waits to be restored (unknown counts as waiting).
  virtual bool displayAtRest()=0;
  // Nodes Multitap can prove are its own: the protected selection's node, the
  // ledger's nodes and the legacy owner node.
  virtual std::vector<Node> ownedNodes()=0;
  // present: still that exact kind of node. foreign: the ID now names something
  // else. unknown: Windows could not be asked.
  virtual Presence inspect(const Node& node)=0;
  // Asks Windows to remove the node; the result is judged by inspect().
  virtual void removeNode(const Node& node)=0;
  virtual void forgetNode(const Node& node)=0;
  // Published driver packages whose INF is byte for byte a pinned original
  // ("oemNN.inf"). nullopt: the driver store could not be read.
  virtual std::optional<std::vector<std::wstring>> packages()=0;
  // Device nodes, present or not, that Windows has bound to this package.
  virtual std::optional<unsigned> devicesUsing(const std::wstring& package)=0;
  // Without force: Windows refuses while a device uses the package.
  virtual PackageRemoval removePackage(const std::wstring& package)=0;
  // Something on this machine still needs the publisher: a device with the
  // driver's hardware ID, or a driver package naming it. nullopt: unknown.
  virtual std::optional<bool> publisherNeeded()=0;
  // The stores this product's own install added the pinned certificate to.
  virtual std::vector<std::string> trustAdded()=0;
  // True when the entry is gone from that store afterwards.
  virtual bool removeCertificate(const std::string& store)=0;
  virtual void forgetTrust(const std::string& store)=0;
  // The provider selection and the display state records; true: none is left.
  virtual bool removeDisplayState()=0;
};

struct Result {
  const char* outcome="skipped";   // skipped | blocked | partial | cleaned
  std::string reason;              // why it was skipped or blocked
  unsigned nodesRemoved=0,nodesGone=0,nodesPending=0,nodesUnknown=0,nodesForeign=0;
  unsigned packagesRemoved=0,packagesKept=0,packagesFailed=0;
  // removed | none (Multitap added nothing: nothing to do) | needed (something
  // still uses the publisher) | nodes (a node of ours remains) | unknown | failed
  const char* certificate="none";
  unsigned certificateEntries=0;   // entries removed
  const char* state="kept";        // kept | removed | failed
  bool foreignLock=false;          // a lock that is not ours was ignored (hostAction 285 `fallback`)
};
// A named lock Multitap checks before it removes anything. Only one that
// exists, can be opened and is owned by SYSTEM or Administrators is an
// installer of ours; any other (a user's object of that name, one that cannot
// be opened, another kind of object) is not ours and blocks nothing.
enum class NamedLock { absent, ours, foreign };
inline NamedLock judgeNamedLock(bool exists, bool opened, bool administrativeOwner){
  if(!exists)return NamedLock::absent;
  return opened&&administrativeOwner?NamedLock::ours:NamedLock::foreign;
}

// Append only: the index is the number Host diagnostics record (hostAction 285
// `error`; server/admin/js/labels.js). 99: a code not listed here.
inline unsigned reasonNumber(const std::string& value){
  static const char* const codes[]={"","arguments","not_removal","upgrade","product","installer_running","service_running","display_restore_pending"};
  for(unsigned index=0;index<sizeof(codes)/sizeof(codes[0]);++index)if(value==codes[index])return index;
  return 99;
}
// hostAction 285 `cause`: why the certificate is as it is.
inline unsigned certificateNumber(const std::string& value){
  static const char* const codes[]={"removed","none","needed","nodes","unknown","failed"};
  for(unsigned index=0;index<sizeof(codes)/sizeof(codes[0]);++index)if(value==codes[index])return index;
  return 99;
}

inline Result cleanup(Access& access,const Request& request){
  Result out;
  // Not a real removal: the machine is not even looked at.
  if(!request.removal){out.reason=request.refusal&&*request.refusal?request.refusal:"arguments";return out;}
  // Only a lock of ours stops the removal; a squatted name never keeps the certificate or the driver behind.
  const bool installer=access.installerRunning();out.foreignLock=access.foreignLockIgnored();
  if(installer){out.reason="installer_running";return out;}
  const auto blocked=[&](const char* reason){out.outcome="blocked";out.reason=reason;return out;};
  if(!access.serviceStopped())return blocked("service_running");
  if(!access.displayAtRest())return blocked("display_restore_pending");

  // 1. Device nodes. One removal request per node; the outcome is what Windows
  //    says afterwards.
  bool native=false;
  for(const auto& node:access.ownedNodes()){
    switch(access.inspect(node)){
      case Presence::absent:++out.nodesGone;access.forgetNode(node);native=native||!node.legacy;break;
      case Presence::foreign:++out.nodesForeign;break;   // not ours any more: never touched
      case Presence::unknown:++out.nodesUnknown;break;
      case Presence::present:
        native=native||!node.legacy;
        access.removeNode(node);
        if(access.inspect(node)==Presence::absent){++out.nodesRemoved;access.forgetNode(node);}
        else ++out.nodesPending;   // Windows finishes it at the next restart, or refused
        break;
    }
  }
  // A node of ours that is still there (or cannot be judged) keeps its driver,
  // its publisher and the selection that names it: a later install finds a
  // consistent machine.
  if(out.nodesPending||out.nodesUnknown){out.outcome="partial";out.certificate="nodes";return out;}

  // 2. The driver package: only when Multitap had a node of this driver, and
  //    only a package no device is bound to.
  bool packagesKnown=true;
  if(native){
    const auto packages=access.packages();
    if(!packages)packagesKnown=false;
    else for(const auto& package:*packages){
      const auto users=access.devicesUsing(package);
      if(!users||*users){++out.packagesKept;continue;}
      switch(access.removePackage(package)){
        case PackageRemoval::removed:++out.packagesRemoved;break;
        case PackageRemoval::inUse:++out.packagesKept;break;
        default:++out.packagesFailed;break;
      }
    }
  }

  // 3. The certificate: only entries Multitap added, only when nothing needs
  //    the publisher any more.
  const auto added=access.trustAdded();
  if(added.empty())out.certificate="none";
  else if(!packagesKnown)out.certificate="unknown";
  else if(out.packagesKept||out.packagesFailed)out.certificate="needed";
  else{
    const auto needed=access.publisherNeeded();
    if(!needed)out.certificate="unknown";
    else if(*needed)out.certificate="needed";
    else{
      bool every=true;
      for(const auto& store:added){
        if(access.removeCertificate(store)){++out.certificateEntries;access.forgetTrust(store);}
        else every=false;
      }
      out.certificate=every?"removed":"failed";
    }
  }

  // 4. The selection and the display state name a device that is gone.
  out.state=access.removeDisplayState()?"removed":"failed";

  const std::string certificate=out.certificate;
  const bool complete=packagesKnown&&!out.packagesFailed&&certificate!="failed"&&certificate!="unknown"&&std::string(out.state)=="removed";
  out.outcome=complete?"cleaned":"partial";
  return out;
}

inline std::string resultJson(const Result& value){
  const auto number=[](unsigned count){return std::to_string(count>64?64u:count);};
  std::string reason;
  for(const char ch:value.reason)if((ch>='a'&&ch<='z')||ch=='_')reason+=ch;
  return std::string("{\"kind\":\"host-removal-cleanup\",\"outcome\":\"")+value.outcome+"\",\"reason\":\""+reason+
    "\",\"nodes\":{\"removed\":"+number(value.nodesRemoved)+",\"gone\":"+number(value.nodesGone)+",\"pending\":"+number(value.nodesPending)+",\"unknown\":"+number(value.nodesUnknown)+",\"foreign\":"+number(value.nodesForeign)+
    "},\"packages\":{\"removed\":"+number(value.packagesRemoved)+",\"kept\":"+number(value.packagesKept)+",\"failed\":"+number(value.packagesFailed)+
    "},\"certificate\":\""+value.certificate+"\",\"certificateEntries\":"+number(value.certificateEntries)+",\"state\":\""+value.state+"\"}";
}
}  // namespace host::removal
