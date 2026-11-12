// host-removal-v1 (host_removal.hpp) and native-display-trust-v1
// (native_display_trust.hpp), on fakes only. No device, driver store,
// certificate store, registry value, file or service is read or changed.
#include "host_removal.hpp"
#include "native_display_trust.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace {
using namespace host::removal;

unsigned checks=0,failed=0;
void check(bool value,const char* name){++checks;if(!value){++failed;std::printf("FAIL %s\n",name);}}
std::string text(const char* value){return value?value:"";}

const std::wstring Ours=L"ROOT\\DISPLAY\\0004",Second=L"ROOT\\DISPLAY\\0002",Sunshine=L"ROOT\\DISPLAY\\0000",Legacy=L"ROOT\\MULTITAPVIRTUALDISPLAY\\0000";
const std::wstring Package=L"oem12.inf",OldPackage=L"oem7.inf";
const std::wstring Product=L"{1AF1997D-C06F-507B-ADC6-31E8D7B1F53B}",Newer=L"{0B1C2D3E-4F50-5A6B-8C7D-9E0F1A2B3C4D}";

// One Windows machine: its device nodes, driver store, the two certificate
// stores, Multitap's records, and the Host's own state.
class Machine final:public Access {
 public:
  struct Device {
    std::wstring package;        // the driver package Windows bound to it
    bool driverHardware=true;    // its hardware ID is the virtual display driver's
    bool removable=true;         // Windows removes it when asked
    bool staysUntilRestart=false;
    bool foreign=false;          // the ID names something that is not that node any more
  };
  std::map<std::wstring,Device> devices;
  std::vector<Node> ledger;                  // what Multitap can prove is its own
  std::set<std::wstring> store{Package};     // pinned packages in the driver store
  std::set<std::wstring> otherPublisherPackages;  // another version of the driver, another product's
  std::set<std::string> certificates{"Root","TrustedPublisher"};
  std::vector<std::string> trust{"Root","TrustedPublisher"};  // what Multitap's install recorded
  bool installer=false,stopped=true,atRest=true,stateFiles=true;
  bool devicesReadable=true,storeReadable=true,packageRemovable=true,windowsRefusesPackage=false,certificatesWritable=true,stateRemovable=true;
  std::vector<std::string> log;   // every change, in order
  unsigned calls=0;               // every question or change at all

  Machine(){devices[Ours]={Package};ledger.push_back({Ours,false});}

  bool installerRunning()override{++calls;return installer;}
  bool foreignLock=false;   // installerRunning() saw a lock that is not ours and ignored it
  bool foreignLockIgnored()override{return foreignLock;}
  bool serviceStopped()override{++calls;return stopped;}
  bool displayAtRest()override{++calls;return atRest;}
  std::vector<Node> ownedNodes()override{++calls;return ledger;}
  Presence inspect(const Node& node)override{
    ++calls;if(!devicesReadable)return Presence::unknown;
    const auto found=devices.find(node.instance);
    return found==devices.end()?Presence::absent:found->second.foreign?Presence::foreign:Presence::present;
  }
  void removeNode(const Node& node)override{
    ++calls;log.push_back("node:"+narrow(node.instance));
    const auto found=devices.find(node.instance);
    if(found!=devices.end()&&found->second.removable&&!found->second.staysUntilRestart)devices.erase(found);
  }
  void forgetNode(const Node& node)override{
    ++calls;log.push_back("forget:"+narrow(node.instance));
    ledger.erase(std::remove_if(ledger.begin(),ledger.end(),[&](const Node& each){return each.instance==node.instance;}),ledger.end());
  }
  std::optional<std::vector<std::wstring>> packages()override{
    ++calls;if(!storeReadable)return std::nullopt;
    return std::vector<std::wstring>(store.begin(),store.end());
  }
  std::optional<unsigned> devicesUsing(const std::wstring& package)override{
    ++calls;if(!devicesReadable)return std::nullopt;
    return unsigned(std::count_if(devices.begin(),devices.end(),[&](const auto& each){return each.second.package==package;}));
  }
  PackageRemoval removePackage(const std::wstring& package)override{
    ++calls;log.push_back("package:"+narrow(package));
    // Windows' own rule: never a package a device is bound to.
    if(windowsRefusesPackage||std::any_of(devices.begin(),devices.end(),[&](const auto& each){return each.second.package==package;}))return PackageRemoval::inUse;
    if(!packageRemovable)return PackageRemoval::failed;
    store.erase(package);return PackageRemoval::removed;
  }
  std::optional<bool> publisherNeeded()override{
    ++calls;if(!devicesReadable||!storeReadable)return std::nullopt;
    return !store.empty()||!otherPublisherPackages.empty()||std::any_of(devices.begin(),devices.end(),[](const auto& each){return each.second.driverHardware;});
  }
  std::vector<std::string> trustAdded()override{++calls;return trust;}
  bool removeCertificate(const std::string& name)override{
    ++calls;log.push_back("certificate:"+name);
    if(certificatesWritable)certificates.erase(name);
    return !certificates.count(name);
  }
  void forgetTrust(const std::string& name)override{++calls;log.push_back("forget-trust:"+name);trust.erase(std::remove(trust.begin(),trust.end(),name),trust.end());}
  bool removeDisplayState()override{++calls;log.push_back("state");if(stateRemovable)stateFiles=false;return !stateFiles;}

  // Nothing at all was changed.
  bool untouched()const{return log.empty()&&devices.count(Ours)&&store.count(Package)&&certificates.size()==2&&trust.size()==2&&stateFiles&&ledger.size()==1;}
  static std::string narrow(const std::wstring& value){std::string out;for(const wchar_t ch:value)out+=char(ch);return out;}
};

// The command line Windows Installer writes for a real removal of `product`
// (apps/windows_host/tests/removal-action.test.ps1 checks the package
// produces exactly these texts).
std::vector<std::wstring> removalArguments(const std::wstring& product=Product){return {L"--remove=ALL",L"--upgrading=",L"--product="+product};}
Request removal(){return parseRequest(removalArguments(),Product);}
std::vector<std::string> list(std::initializer_list<const char*> values){return std::vector<std::string>(values.begin(),values.end());}

void requests(){
  check(removal().removal&&text(removal().refusal).empty(),"the removal command line is a removal");
  // An update: Windows Installer removes the old product for the new one.
  {const auto upgrade=parseRequest({L"--remove=ALL",L"--upgrading="+Newer,L"--product="+Product},Product);
   check(!upgrade.removal&&text(upgrade.refusal)=="upgrade","the removal that belongs to an update is refused");}
  // Windows Installer did not resolve the property: unknown is never a removal.
  {const auto literal=parseRequest({L"--remove=ALL",L"--upgrading=[UPGRADINGPRODUCTCODE]",L"--product="+Product},Product);
   check(!literal.removal&&text(literal.refusal)=="upgrade","an unresolved upgrade property is refused");
   const auto space=parseRequest({L"--remove=ALL",L"--upgrading= ",L"--product="+Product},Product);
   check(!space.removal&&text(space.refusal)=="upgrade","anything after --upgrading= is refused");}
  // Not the removal of the whole product.
  for(const auto* remove:{L"--remove=",L"--remove=HostFeature",L"--remove=all",L"--remove=[REMOVE]",L"--remove=ALL "}){
    const auto partial=parseRequest({remove,L"--upgrading=",L"--product="+Product},Product);
    check(!partial.removal&&text(partial.refusal)=="not_removal","only REMOVE=ALL is a removal");
  }
  // Another product's code, an unreadable install, a malformed code.
  check(text(parseRequest(removalArguments(Newer),Product).refusal)=="product"&&!parseRequest(removalArguments(Newer),Product).removal,"another product code is refused");
  check(!parseRequest(removalArguments(),L"").removal&&!parseRequest(removalArguments(L""),L"").removal&&!parseRequest(removalArguments(L"[ProductCode]"),L"[ProductCode]").removal,"without the installed product code nothing is a removal");
  check(!parseRequest(removalArguments(L"{1af1997d-c06f-507b-adc6-31e8d7b1f53b}"),L"{1af1997d-c06f-507b-adc6-31e8d7b1f53b}").removal,"a product code is exactly as Windows Installer writes it");
  // Missing, extra, reordered or renamed arguments.
  const std::vector<std::vector<std::wstring>> malformed{{},{L"--remove=ALL"},{L"--remove=ALL",L"--upgrading="},{L"--upgrading=",L"--remove=ALL",L"--product="+Product},
    {L"--remove=ALL",L"--upgrading=",L"--product="+Product,L"--force"},{L"--remove=ALL",L"--product="+Product,L"--upgrading="},{L"--REMOVE=ALL",L"--upgrading=",L"--product="+Product},
    {L"ALL",L"",Product},{L"--remove=ALL --upgrading= --product="+Product}};
  for(const auto& arguments:malformed){const auto request=parseRequest(arguments,Product);check(!request.removal&&text(request.refusal)=="arguments","a command line the package does not write is refused");}
  check(productCode(Product)&&!productCode(L"")&&!productCode(L"{1AF1997D-C06F-507B-ADC6-31E8D7B1F53B")&&!productCode(L"1AF1997D-C06F-507B-ADC6-31E8D7B1F53B00")&&!productCode(L"{1AF1997D_C06F-507B-ADC6-31E8D7B1F53B}")&&!productCode(L"{1AF1997G-C06F-507B-ADC6-31E8D7B1F53B}"),"product code shape");
}

void upgrades(){
  // THE dangerous case: every refused request leaves the machine unasked and unchanged.
  const std::vector<std::vector<std::wstring>> refused{{L"--remove=ALL",L"--upgrading="+Newer,L"--product="+Product},{L"--remove=ALL",L"--upgrading=[UPGRADINGPRODUCTCODE]",L"--product="+Product},
    {L"--remove=",L"--upgrading=",L"--product="+Product},{L"--remove=ALL",L"--upgrading=",L"--product="+Newer},{},{L"--remove=ALL",L"--upgrading=",L"--product="+Product,L"--extra"}};
  for(const auto& arguments:refused){
    Machine m;const auto result=cleanup(m,parseRequest(arguments,Product));
    check(text(result.outcome)=="skipped"&&m.calls==0&&m.untouched()&&!result.reason.empty(),"a run that is not a real removal neither asks nor changes anything");
  }
  {Machine m;const auto result=cleanup(m,parseRequest({L"--remove=ALL",L"--upgrading="+Newer,L"--product="+Product},Product));
   check(result.reason=="upgrade"&&resultJson(result)=="{\"kind\":\"host-removal-cleanup\",\"outcome\":\"skipped\",\"reason\":\"upgrade\",\"nodes\":{\"removed\":0,\"gone\":0,\"pending\":0,\"unknown\":0,\"foreign\":0},\"packages\":{\"removed\":0,\"kept\":0,\"failed\":0},\"certificate\":\"none\",\"certificateEntries\":0,\"state\":\"kept\"}","an update reports that it was skipped as an update");}
  // A default request is not a removal either.
  {Machine m;const auto result=cleanup(m,Request{});check(text(result.outcome)=="skipped"&&m.calls==0&&m.untouched(),"a request nobody parsed is refused");}
  // The Multitap installer is running (install or update): even a command line
  // that says removal changes nothing.
  {Machine m;m.installer=true;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="skipped"&&result.reason=="installer_running"&&m.calls==1&&m.untouched(),"nothing is cleaned while the Multitap installer runs");}
}

void removals(){
  // A normal Host: one node, its package, the certificate Multitap added, the selection.
  {Machine m;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.reason.empty()&&result.nodesRemoved==1&&result.packagesRemoved==1&&text(result.certificate)=="removed"&&result.certificateEntries==2&&text(result.state)=="removed","a real removal takes everything Multitap installed");
   check(m.devices.empty()&&m.store.empty()&&m.certificates.empty()&&m.trust.empty()&&!m.stateFiles&&m.ledger.empty(),"nothing of Multitap's is left");
   check(m.log==list({"node:ROOT\\DISPLAY\\0004","forget:ROOT\\DISPLAY\\0004","package:oem12.inf","certificate:Root","forget-trust:Root","certificate:TrustedPublisher","forget-trust:TrustedPublisher","state"}),"the node first, then its package, then the trust, then the records");
   check(resultJson(result)=="{\"kind\":\"host-removal-cleanup\",\"outcome\":\"cleaned\",\"reason\":\"\",\"nodes\":{\"removed\":1,\"gone\":0,\"pending\":0,\"unknown\":0,\"foreign\":0},\"packages\":{\"removed\":1,\"kept\":0,\"failed\":0},\"certificate\":\"removed\",\"certificateEntries\":2,\"state\":\"removed\"}","result shape");}
  // A second recorded node (an interrupted earlier install) and both driver versions in the store.
  {Machine m;m.devices[Second]={OldPackage};m.ledger.push_back({Second,false});m.store.insert(OldPackage);const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.nodesRemoved==2&&result.packagesRemoved==2&&m.devices.empty()&&m.store.empty()&&m.certificates.empty(),"every recorded node and every pinned package goes");}
  // The node is already gone (removed in Device Manager): the rest still goes.
  {Machine m;m.devices.clear();const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.nodesRemoved==0&&result.nodesGone==1&&result.packagesRemoved==1&&text(result.certificate)=="removed"&&m.store.empty()&&m.certificates.empty()&&!m.stateFiles&&m.ledger.empty(),"a node that is already gone does not stop the rest");
   check(std::find(m.log.begin(),m.log.end(),"node:ROOT\\DISPLAY\\0004")==m.log.end(),"a node that is gone is not removed again");}
  // The certificate was trusted before Multitap (nothing recorded): it stays.
  {Machine m;m.trust.clear();const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&text(result.certificate)=="none"&&result.certificateEntries==0&&m.certificates.size()==2&&m.devices.empty()&&m.store.empty()&&!m.stateFiles,"a certificate that was there before Multitap stays");
   check(std::none_of(m.log.begin(),m.log.end(),[](const std::string& each){return each.rfind("certificate:",0)==0;}),"a certificate Multitap did not add is not even asked for");}
  // Multitap added one of the two entries: only that one goes.
  {Machine m;m.trust={"TrustedPublisher"};const auto result=cleanup(m,removal());
   check(text(result.certificate)=="removed"&&result.certificateEntries==1&&m.certificates==std::set<std::string>({"Root"})&&m.trust.empty(),"only the entry Multitap added is removed");}
  // Another device uses the same driver (Sunshine's own display): its node, the
  // package and the certificate stay; only Multitap's node and records go.
  {Machine m;m.devices[Sunshine]={Package};const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.nodesRemoved==1&&result.packagesRemoved==0&&result.packagesKept==1&&text(result.certificate)=="needed"&&result.certificateEntries==0,"a driver another device uses is kept");
   check(m.devices.size()==1&&m.devices.count(Sunshine)&&m.store.count(Package)&&m.certificates.size()==2&&m.trust.size()==2&&!m.stateFiles,"the other product's device keeps its driver and its publisher");
   check(std::none_of(m.log.begin(),m.log.end(),[](const std::string& each){return each.rfind("package:",0)==0||each.rfind("certificate:",0)==0||each=="node:ROOT\\DISPLAY\\0000";}),"neither the shared package nor the certificate nor the other node is touched");}
  // ... even if this code miscounted, Windows itself refuses the package.
  {Machine m;m.windowsRefusesPackage=true;const auto result=cleanup(m,removal());
   check(result.packagesKept==1&&result.packagesRemoved==0&&text(result.certificate)=="needed"&&m.store.count(Package)&&m.certificates.size()==2,"a package Windows says is in use keeps its publisher");}
  // Another product ships its own version of the driver: our package goes, the publisher stays.
  {Machine m;m.otherPublisherPackages.insert(L"oem30.inf");const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.packagesRemoved==1&&text(result.certificate)=="needed"&&m.certificates.size()==2&&m.trust.size()==2,"another driver package of the same publisher keeps the certificate");}
  // ... or a device of that driver without a package bound yet.
  {Machine m;m.devices[Sunshine]={L""};const auto result=cleanup(m,removal());
   check(result.packagesRemoved==1&&text(result.certificate)=="needed"&&m.certificates.size()==2&&m.devices.count(Sunshine),"another device of the driver keeps the certificate");}
}

void blocked(){
  // A stream is running: the service has not stopped. Nothing is removed.
  {Machine m;m.stopped=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="blocked"&&result.reason=="service_running"&&m.untouched()&&m.calls==2,"nothing is removed while the Host service runs");}
  // The service stopped but the stream's display layout was not put back.
  {Machine m;m.atRest=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="blocked"&&result.reason=="display_restore_pending"&&m.untouched()&&m.calls==3,"nothing is removed while a display layout waits to be restored");}
  // Windows keeps the node until a restart: its driver, publisher and selection stay with it.
  {Machine m;m.devices[Ours].staysUntilRestart=true;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&result.nodesPending==1&&result.nodesRemoved==0&&text(result.certificate)=="nodes"&&text(result.state)=="kept","a node that is still there stops the cleanup");
   check(m.log==list({"node:ROOT\\DISPLAY\\0004"})&&m.store.count(Package)&&m.certificates.size()==2&&m.trust.size()==2&&m.stateFiles&&m.ledger.size()==1,"a remaining node keeps its driver, its publisher, its record and the selection");}
  {Machine m;m.devices[Ours].removable=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&result.nodesPending==1&&m.certificates.size()==2&&m.stateFiles,"a node Windows refuses to remove keeps everything");}
  // One of two nodes stays: the other is removed, nothing after that.
  {Machine m;m.devices[Second]={Package};m.devices[Second].staysUntilRestart=true;m.ledger.push_back({Second,false});const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&result.nodesRemoved==1&&result.nodesPending==1&&m.store.count(Package)&&m.certificates.size()==2&&m.stateFiles&&m.ledger.size()==1&&m.ledger[0].instance==Second,"one remaining node keeps the shared pieces");}
  // Windows cannot be asked about devices: unknown is never "gone".
  {Machine m;m.devicesReadable=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&result.nodesUnknown==1&&m.untouched(),"an unknown device state removes nothing");}
  // The ID names something else now: never touched, and it does not hold up the rest.
  {Machine m;m.devices[Ours].foreign=true;m.devices[Ours].driverHardware=false;m.devices[Ours].package=L"oem99.inf";const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.nodesForeign==1&&result.nodesRemoved==0&&m.devices.count(Ours)&&m.ledger.size()==1&&!m.stateFiles,"a device that is not ours any more is left alone");
   check(std::none_of(m.log.begin(),m.log.end(),[](const std::string& each){return each.rfind("node:",0)==0||each.rfind("package:",0)==0;}),"no node of ours was seen, so no package is removed either");}
  // The driver store cannot be read: the publisher stays.
  {Machine m;m.storeReadable=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&result.nodesRemoved==1&&text(result.certificate)=="unknown"&&m.certificates.size()==2&&m.trust.size()==2&&!m.stateFiles,"an unreadable driver store keeps the certificate");}
  // The package cannot be deleted: it may still be used, so the publisher stays.
  {Machine m;m.packageRemovable=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&result.packagesFailed==1&&text(result.certificate)=="needed"&&m.certificates.size()==2&&m.store.count(Package),"a package that could not be deleted keeps its publisher");}
  // A certificate store refuses: reported, and the record of that entry stays.
  {Machine m;m.certificatesWritable=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&text(result.certificate)=="failed"&&result.certificateEntries==0&&m.trust.size()==2&&m.certificates.size()==2&&!m.stateFiles,"an entry that could not be removed stays recorded");}
  // The records cannot be deleted.
  {Machine m;m.stateRemovable=false;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="partial"&&text(result.state)=="failed"&&m.devices.empty()&&m.certificates.empty(),"records that stay are reported");}
  // No node of Multitap's is known (the install was never finished): a package
  // in the store is not provably ours and keeps the publisher.
  {Machine m;m.devices.clear();m.ledger.clear();const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.packagesRemoved==0&&m.store.count(Package)&&text(result.certificate)=="needed"&&m.certificates.size()==2&&!m.stateFiles,"without a node of ours no driver package is removed");
   check(std::none_of(m.log.begin(),m.log.end(),[](const std::string& each){return each.rfind("package:",0)==0;}),"the driver store is not searched without a node of ours");}
  // A legacy node (the earlier Multitap driver) is removed; it is not a reason to look for the native package.
  {Machine m;m.devices.clear();m.ledger.clear();m.store.clear();m.devices[Legacy]={L"oem3.inf",false};m.ledger.push_back({Legacy,true});const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.nodesRemoved==1&&m.devices.empty()&&text(result.certificate)=="removed"&&m.log.front()=="node:ROOT\\MULTITAPVIRTUALDISPLAY\\0000","a legacy node of ours is removed too");}
  // The numbers Host diagnostics record are fixed.
  check(reasonNumber("")==0&&reasonNumber("upgrade")==3&&reasonNumber("installer_running")==5&&reasonNumber("service_running")==6&&reasonNumber("display_restore_pending")==7&&reasonNumber("x")==99,"reason numbers are append-only");
  check(certificateNumber("removed")==0&&certificateNumber("none")==1&&certificateNumber("needed")==2&&certificateNumber("nodes")==3&&certificateNumber("unknown")==4&&certificateNumber("failed")==5&&certificateNumber("x")==99,"certificate numbers are append-only");
  {Result leaked;leaked.reason="C:\\Users\\name\"";check(resultJson(leaked).find("\"reason\":\"sersname\",")!=std::string::npos&&resultJson(leaked).find('\\')==std::string::npos,"a reason is reduced to lower-case letters");}
}

// native-display-trust-v1: copying the journal's note of an added certificate
// into the durable record, once per machine.
void trust(){
  using host::journalLineAddedTrust;
  const std::string added=R"({"time":"2026-11-10T03:21:09.120Z","event":"hostAction","values":{"action":276,"stage":1,"ok":true,"error":0,"installed":true,"started":true,"reboot":false,"changed":true,"restartAttempted":false,"running":true,"recovery":false}})";
  const std::string present=R"({"time":"2026-11-10T03:21:09.120Z","event":"hostAction","values":{"action":276,"stage":1,"ok":true,"error":0,"installed":true,"started":true,"reboot":false,"changed":false,"restartAttempted":false,"running":true,"recovery":false}})";
  check(journalLineAddedTrust(added)&&!journalLineAddedTrust(present),"only a run that added the certificate counts");
  check(!journalLineAddedTrust(R"({"time":"2026-11-10T03:21:09.120Z","event":"hostAction","values":{"action":276,"stage":1,"ok":false,"error":17,"changed":true,"recovery":false}})"),"a failed run does not count");
  check(!journalLineAddedTrust(R"({"time":"2026-11-10T03:21:09.120Z","event":"hostAction","values":{"action":276,"stage":2,"ok":true,"changed":true}})")&&!journalLineAddedTrust(R"({"time":"2026-11-10T03:21:09.120Z","event":"hostAction","values":{"action":216,"ok":true,"changed":true}})")&&
    !journalLineAddedTrust(R"({"time":"2026-11-10T03:21:09.120Z","event":"driverInstall","values":{"action":276,"stage":1,"ok":true,"changed":true}})"),"another step, action or event does not count");
  check(!journalLineAddedTrust("")&&!journalLineAddedTrust("not json")&&!journalLineAddedTrust(R"({"event":"hostAction","values":{"action":276,"stage":1,"ok":1,"changed":1}})")&&!journalLineAddedTrust(std::string(3000,' ')),"a damaged line does not count");

  struct Registry {
    bool checked=false,journalReadable=true,recordable=true;
    std::vector<std::string> record,journal;std::set<std::string> certificates{"Root","TrustedPublisher"};
    unsigned journalReads=0;
    host::NativeDisplayTrustBackfill access(){
      host::NativeDisplayTrustBackfill out;
      out.checked=[this]{return checked;};
      out.recorded=[this]{return record;};
      out.journal=[this]()->std::optional<std::vector<std::string>>{++journalReads;if(!journalReadable)return std::nullopt;return journal;};
      out.present=[this](const std::string& name){return certificates.count(name)!=0;};
      out.record=[this](const std::string& name){if(!recordable)return false;record.push_back(name);return true;};
      out.markChecked=[this]{checked=true;return true;};
      return out;
    }
  };
  // An earlier Host on a clean Windows: the journal still has the line.
  {Registry r;r.journal={present,"{\"other\":1}",added};const auto copied=host::backfillNativeDisplayTrust(r.access());
   check(copied==list({"Root","TrustedPublisher"})&&r.record==list({"Root","TrustedPublisher"})&&r.checked,"an added certificate noted in the journal becomes a durable record");
   const auto again=host::backfillNativeDisplayTrust(r.access());check(again.empty()&&r.record.size()==2&&r.journalReads==1,"the journal is looked at once");}
  // The journal says the certificate was there already (Sunshine was installed first), or says nothing.
  {Registry r;r.journal={present};check(host::backfillNativeDisplayTrust(r.access()).empty()&&r.record.empty()&&r.checked,"a certificate that was there before is not claimed");}
  {Registry r;check(host::backfillNativeDisplayTrust(r.access()).empty()&&r.record.empty()&&r.checked,"an empty journal claims nothing");}
  // After a removal the mark is still there: an old journal line can no longer
  // claim a certificate somebody else added afterwards.
  {Registry r;r.checked=true;r.journal={added};check(host::backfillNativeDisplayTrust(r.access()).empty()&&r.record.empty()&&r.journalReads==0,"a checked machine never claims from the journal again");}
  // The install itself wrote the record (this version onward): never extended from the journal.
  {Registry r;r.record={"TrustedPublisher"};r.journal={added};check(host::backfillNativeDisplayTrust(r.access()).empty()&&r.record==list({"TrustedPublisher"})&&r.checked&&r.journalReads==0,"an existing record is not extended");}
  // Unreadable now: asked again next time, nothing claimed.
  {Registry r;r.journalReadable=false;r.journal={added};check(host::backfillNativeDisplayTrust(r.access()).empty()&&r.record.empty()&&!r.checked,"an unreadable journal is not a decision");
   r.journalReadable=true;check(host::backfillNativeDisplayTrust(r.access()).size()==2&&r.checked,"it is read when it can be");}
  // Only entries that exist now are recorded.
  {Registry r;r.journal={added};r.certificates={"TrustedPublisher"};check(host::backfillNativeDisplayTrust(r.access())==list({"TrustedPublisher"})&&r.record==list({"TrustedPublisher"}),"an entry that is not there is not recorded");}
  {Registry r;r.journal={added};r.recordable=false;check(host::backfillNativeDisplayTrust(r.access()).empty()&&r.record.empty(),"a record that could not be written is not reported");}
  check(host::backfillNativeDisplayTrust(host::NativeDisplayTrustBackfill{}).empty(),"without access nothing happens");
  check(host::nativeDisplayThumbprint("37E100B2B382866FA7CE04EE33FE53DF53AEDC6D")&&!host::nativeDisplayThumbprint("37e100b2b382866fa7ce04ee33fe53df53aedc6d")&&!host::nativeDisplayThumbprint("37E1")&&host::nativeDisplayTrustStoreName("Root")&&host::nativeDisplayTrustStoreName("TrustedPublisher")&&!host::nativeDisplayTrustStoreName("My")&&!host::nativeDisplayTrustStoreName("CA"),"only the two stores and an exact thumbprint are ever named");
}
}

// Any user can create Global\MultitapHostInstaller
// (or the device-install lock) first. Only a lock of ours (opened, owned by
// SYSTEM or Administrators) is an installer; any other is ignored and never
// keeps the certificate, the driver or the records behind.
void squattedLocks(){
  check(judgeNamedLock(false,false,false)==NamedLock::absent,"no lock is no installer");
  check(judgeNamedLock(true,true,true)==NamedLock::ours,"an administrator-owned lock is an installer of ours");
  check(judgeNamedLock(true,true,false)==NamedLock::foreign,"a user-owned lock is not ours");
  check(judgeNamedLock(true,false,false)==NamedLock::foreign&&judgeNamedLock(true,false,true)==NamedLock::foreign,"a lock that cannot be opened is not ours");
  {Machine m;m.foreignLock=true;const auto result=cleanup(m,removal());
   check(text(result.outcome)=="cleaned"&&result.foreignLock&&text(result.certificate)=="removed"&&m.certificates.empty()&&m.trust.empty()&&m.devices.empty()&&!m.stateFiles,
     "a squatted lock name does not stop the removal of the certificate, the driver and the records");}
  {Machine m;m.installer=true;const auto result=cleanup(m,removal());
   check(result.reason=="installer_running"&&!result.foreignLock&&m.certificates.size()==2,"a real installer of ours still stops the removal");}
}
int main(){
  requests();upgrades();removals();blocked();trust();squattedLocks();
  std::printf("checks=%u failed=%u\n",checks,failed);
  return failed?1:0;
}
