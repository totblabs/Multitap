// host-prerequisites-v1 (host_prerequisites.hpp): the native display finish of a
// Host install, on fakes only. No SetupAPI/CfgMgr backend, certificate store,
// file, registry value, device or service is read or changed.
#define MULTITAP_SETUP_QA
#include "host_prerequisites.hpp"
#include <algorithm>
#include <cstdio>
#include <map>

namespace {
using namespace host::prerequisites;
using multitap::pnp::Driver165InfSha;

unsigned checks=0,failed=0;
void check(bool value,const char* name){++checks;if(!value){++failed;std::printf("FAIL %s\n",name);}}

const std::wstring Created=L"ROOT\\DISPLAY\\0007",Selected=L"ROOT\\DISPLAY\\0001";

// A machine's device nodes. `installCode`: what the install returns; a failure
// after registration may leave its node (`leaves`) and may name it (`names`).
class Pnp final:public multitap::pnp::Backend {
 public:
  std::map<std::wstring,multitap::pnp::Status> nodes;
  std::string installCode,presence;  // presence: "unknown"/"foreign" for every inspection
  bool leaves=false,names=true,removable=true,restart=false;
  size_t installs=0,removes=0,labels=0,inspections=0;
  multitap::pnp::Status status(const std::wstring& id)override{const auto found=nodes.find(id);return found==nodes.end()?multitap::pnp::Status{}:found->second;}
  multitap::pnp::Inspection inspect(const std::wstring& id)override{
    ++inspections;multitap::pnp::Inspection out;
    if(!presence.empty()){out.presence=presence;return out;}
    const auto found=nodes.find(id);out.presence=found==nodes.end()?"absent":"present";if(found!=nodes.end())out.status=found->second;return out;
  }
  std::string install(const std::wstring&,std::wstring& instance,bool& requested)override{
    ++installs;instance.clear();requested=false;
    if(installCode.empty()){nodes[Created]={0xAUL|(restart?0x100UL:0UL),0};instance=Created;requested=restart;return {};}
    if(installCode=="pnp_install_failed"){if(leaves)nodes[Created]={0,28};if(names)instance=Created;}
    return installCode;
  }
  std::string label(const std::wstring&)override{++labels;return "pnp_label_failed";}  // cosmetic: must not matter
  bool remove(const std::wstring& id)override{++removes;if(!removable||!nodes.count(id))return false;nodes.erase(id);return true;}
};

class Machine final:public Access {
 public:
  Pnp devices;
  std::string packageCode,bound=Driver165InfSha,written;
  bool root=false,publisher=false,storeWritable=true,catalogAfterTrust=true,selectionWritable=true,serviceRestarts=true;
  Selection current;
  std::vector<std::wstring> recorded;        // the ledger
  std::map<std::wstring,std::string> boundOf;  // a node bound to something else than `bound`
  std::vector<std::string> log;
  size_t restarts=0,waits=0,uses=0;
  unsigned long waitedMs=0;
  // The running service: it holds the selected node after `adoptAfter` waits
  // (0: at once; -1: never by itself). A completed restart makes
  // it hold the node.
  int adoptAfter=0;
  bool restarted=false;
  std::string package()override{return packageCode;}
  bool certificateTrusted()override{return root&&publisher;}
  bool catalogTrusted()override{return root&&catalogAfterTrust;}
  bool addCertificate(std::vector<std::string>& added)override{
    if(!root){if(!storeWritable)return false;root=true;added.push_back("Root");log.push_back("add:Root");}
    if(!publisher){publisher=true;added.push_back("TrustedPublisher");log.push_back("add:TrustedPublisher");}
    return true;
  }
  void removeCertificate(const std::string& store)override{log.push_back("remove:"+store);if(store=="Root")root=false;if(store=="TrustedPublisher")publisher=false;}
  // The durable record (native_display_trust.hpp): `trust` is what it holds,
  // `trustLog` every write in order. An entry must exist when it is recorded.
  std::vector<std::string> trust,trustLog;bool recordedWithoutEntry=false;
  void recordTrust(const std::string& store)override{trustLog.push_back("record:"+store);if(!(store=="Root"?root:store=="TrustedPublisher"?publisher:false))recordedWithoutEntry=true;if(std::find(trust.begin(),trust.end(),store)==trust.end())trust.push_back(store);}
  void forgetTrust(const std::string& store)override{trustLog.push_back("forget:"+store);trust.erase(std::remove(trust.begin(),trust.end(),store),trust.end());}
  Selection selection()override{return current;}
  std::vector<std::wstring> recordedNodes()override{return recorded;}
  multitap::pnp::Backend& pnp()override{return devices;}
  std::wstring inf()override{return L"C:\\Program Files\\Multitap\\Host\\bin\\native-display-driver\\SunshineVirtualDisplayDriver.inf";}
  std::string boundInf(const std::wstring& id)override{const auto found=boundOf.find(id);return found==boundOf.end()?bound:found->second;}
  bool writeSelection(const std::string& json)override{if(!selectionWritable)return false;written=json;log.push_back("selection");return true;}
  bool serviceUsesDevice(const std::wstring& id)override{
    ++uses;
    std::string narrow;for(const wchar_t ch:id){if(ch==L'\\')narrow+="\\\\";else narrow+=char(ch);}
    if(written.find("\"deviceInstanceId\":\""+narrow+"\"")==std::string::npos)return false;  // only the node the selection names
    return restarted||(adoptAfter>=0&&waits>=size_t(adoptAfter));
  }
  void wait(unsigned long milliseconds)override{++waits;waitedMs+=milliseconds;}
  bool restartService()override{++restarts;log.push_back("service");if(serviceRestarts)restarted=true;return serviceRestarts;}
  // What Host settings is told while it waits (settings_progress.hpp); never part of `log`:
  // telling a step changes nothing.
  std::vector<std::string> steps;
  void step(const char* code)override{steps.push_back(code?code:"");}
  bool changed()const{return !log.empty()||devices.installs||devices.removes;}
};
Selection selected(const std::wstring& instance=Selected){Selection value;value.present=value.valid=true;value.instance=instance;return value;}
std::string text(const char* value){return value?value:"";}
}

int main(){
  // A clean Windows: nothing trusted, nothing selected, no node.
  {
    Machine m;const auto before=observe(m);
    check(text(before.state)=="install"&&text(before.selection)=="none"&&text(before.device)=="none"&&!before.certificateTrusted&&before.reason.empty(),"a clean machine is installable");
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.action)=="installed"&&text(result.certificate)=="added"&&text(result.service)=="adopted"&&result.deviceReady&&!result.restart&&!result.rolledBack&&!result.restartAttempted&&result.instance==Created,"first install completes");
    check(m.root&&m.publisher&&m.devices.installs==1&&m.devices.removes==0&&m.devices.labels==1&&m.restarts==0&&m.waits==0&&m.uses==1,"one certificate pair, one node, and a service that took it is not restarted");
    check(m.log==std::vector<std::string>({"add:Root","add:TrustedPublisher","selection"}),"trust, then the node, then the selection");
    check(m.written==std::string("{\"provider\":\"libvirtualdisplay-1.6.4\",\"enabled\":true,\"deviceInstanceId\":\"ROOT\\\\DISPLAY\\\\0007\",\"monitors\":2,\"viewportTrialEnabled\":true,\"streamProfileTrialEnabled\":true,\"streamProfileTracksTrialEnabled\":true,\"driverInfSha256\":\"")+Driver165InfSha+"\",\"driverPackageVersion\":\"1.6.5\"}","the selection the earlier installer wrote");
    m.current=selected(Created);
    const auto after=observe(m);
    check(text(after.state)=="ready"&&text(after.device)=="ready"&&after.certificateTrusted,"the finished machine is ready");
    check(resultJson(result)=="{\"action\":\"installed\",\"error\":\"\",\"restart\":false,\"deviceReady\":true,\"certificate\":\"added\",\"service\":\"adopted\",\"rolledBack\":false}","result reply shape");
    check(observationJson(before,true,false,false,true)=="{\"schema\":1,\"administrator\":true,\"remoteSession\":false,\"display\":{\"state\":\"install\",\"reason\":\"\",\"package\":\"intact\",\"certificate\":\"untrusted\",\"selection\":\"none\",\"device\":\"none\"},\"winfsp\":{\"installed\":false,\"package\":true},\"restartRequired\":false}","status reply shape");
  }
  // No confirmation, or no consent for the certificate: nothing changes.
  {
    Machine m;const auto unconfirmed=install(m,false,true);
    check(unconfirmed.error=="confirmation_required"&&text(unconfirmed.action)=="skipped"&&!m.changed(),"no confirmation changes nothing");
    const auto declined=install(m,true,false);
    check(declined.error=="certificate_trust_required"&&text(declined.certificate)=="unchanged"&&!m.root&&!m.publisher&&!m.changed(),"declined trust adds no certificate and no device");
  }
  // The certificate was trusted earlier: no trust consent is needed, nothing is added.
  {
    Machine m;m.root=m.publisher=true;
    const auto result=install(m,true,false);
    check(result.error.empty()&&text(result.action)=="installed"&&text(result.certificate)=="present"&&m.log==std::vector<std::string>({"selection"}),"an already trusted publisher is used as it is");
  }
  // ... but Windows refuses the catalog anyway: stopped unchanged, with or without a trust consent.
  for(const bool consent:{false,true}){
    Machine m;m.root=m.publisher=true;m.catalogAfterTrust=false;
    const auto result=install(m,true,consent);
    check(result.error=="native_display_catalog_untrusted"&&text(result.certificate)=="present"&&m.root&&m.publisher&&!m.changed(),"a refused catalog under an existing trust changes nothing");
  }
  // One store has it, the other not: only the missing entry is added, and only that one is undone.
  {
    Machine m;m.root=true;m.devices.installCode="pnp_driver_unavailable";
    const auto result=install(m,true,true);
    check(result.error=="pnp_driver_unavailable"&&result.rolledBack&&m.root&&!m.publisher&&text(result.certificate)=="removed","a certificate that was there before stays after a failure");
    check(m.log==std::vector<std::string>({"add:TrustedPublisher","remove:TrustedPublisher"})&&m.written.empty()&&m.restarts==0,"only this run's entry is removed");
  }
  // The selected node is there: kept, whatever its state. No trust, node, file or service change.
  {
    Machine m;m.current=selected();m.devices.nodes[Selected]={0xA,0};
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.action)=="kept"&&result.deviceReady&&!result.restart&&result.instance==Selected&&!m.changed()&&!m.root,"an installed device is a no-op");
    m.devices.nodes[Selected]={0x10A,0};
    const auto waiting=install(m,true,true);
    check(text(waiting.action)=="kept"&&waiting.restart&&!m.changed()&&text(observe(m).state)=="restart","a kept node that waits for a restart says so");
    m.devices.nodes[Selected]={0,22};
    const auto stopped=observe(m);
    check(text(stopped.state)=="attention"&&stopped.reason=="native_display_device_not_started"&&text(install(m,true,true).action)=="kept"&&!m.changed(),"a stopped selected node is reported, not replaced");
  }
  // The selected node is provably gone (VM restored from a template): a new node, the selection replaced.
  {
    Machine m;m.current=selected();m.current.displayManager="v1";
    check(text(observe(m).state)=="install"&&text(observe(m).device)=="absent","a gone selected node is installable");
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.action)=="installed"&&result.instance==Created&&m.written.find("\"deviceInstanceId\":\"ROOT\\\\DISPLAY\\\\0007\"")!=std::string::npos&&m.written.find(",\"displayManager\":\"v1\"}")!=std::string::npos,"the selection moves to the new node and keeps the display manager choice");
  }
  // Unknown or foreign: never permission to create another node.
  for(const auto* presence:{"unknown","foreign"}){
    Machine m;m.current=selected();m.devices.presence=presence;
    const auto status=observe(m);const auto result=install(m,true,true);
    check(text(status.state)=="attention"&&status.reason=="native_selected_device_unverified"&&result.error=="native_selected_device_unverified"&&!m.changed(),"an unverified selected node stops the install");
  }
  // A selection this code does not understand is left for an administrator.
  {
    Machine m;m.current.present=true;
    const auto status=observe(m);const auto result=install(m,true,true);
    check(text(status.state)=="attention"&&text(status.selection)=="invalid"&&result.error=="native_selection_invalid"&&!m.changed(),"an unreadable selection is never replaced");
  }
  // The package is not the pinned original: nothing is trusted or installed.
  {
    Machine m;m.packageCode="native_display_package_mismatch";
    const auto status=observe(m);const auto result=install(m,true,true);
    check(text(status.state)=="attention"&&status.reason=="native_display_package_mismatch"&&result.error=="native_display_package_mismatch"&&!m.changed(),"a changed package is refused before any trust change");
    check(observationJson(status,false,true,true,false).find("\"package\":\"native_display_package_mismatch\"")!=std::string::npos,"the package code is reported");
  }
  // The store cannot be written, or Windows still refuses the catalog: this run's entries are removed again.
  {
    Machine m;m.storeWritable=false;
    const auto result=install(m,true,true);
    check(result.error=="certificate_store_failed"&&!m.root&&!m.publisher&&m.devices.installs==0,"a store that cannot be written leaves no partial trust");
    Machine n;n.catalogAfterTrust=false;
    const auto untrusted=install(n,true,true);
    check(untrusted.error=="native_display_catalog_untrusted"&&!n.root&&!n.publisher&&n.devices.installs==0&&text(untrusted.certificate)=="removed","an unverified catalog withdraws the added trust and installs nothing");
  }
  // PnP failure before registration: no node exists, the added trust is removed.
  for(const auto* reason:{"pnp_inf_invalid","pnp_unavailable","pnp_create_failed","pnp_driver_unavailable","pnp_register_failed"}){
    Machine m;m.devices.installCode=reason;
    const auto result=install(m,true,true);
    check(result.error==reason&&result.rolledBack&&!m.root&&!m.publisher&&m.written.empty()&&m.restarts==0&&m.devices.nodes.empty()&&m.devices.removes==0,"a failed create is rolled back completely");
  }
  // PnP failure after registration, node already removed by the backend: rolled back.
  {
    Machine m;m.devices.installCode="pnp_install_failed";
    const auto result=install(m,true,true);
    check(result.error=="pnp_install_failed"&&result.rolledBack&&!m.root&&!m.publisher&&m.devices.removes==0&&m.written.empty(),"a failed driver install whose node is gone is rolled back");
  }
  // ... the node is still there: removed once, then the trust goes.
  {
    Machine m;m.devices.installCode="pnp_install_failed";m.devices.leaves=true;
    const auto result=install(m,true,true);
    check(result.error=="pnp_install_failed"&&result.rolledBack&&m.devices.removes==1&&m.devices.nodes.empty()&&!m.root&&!m.publisher,"a left node is removed before the trust");
  }
  // ... the node cannot be removed: the approved publisher stays with it.
  {
    Machine m;m.devices.installCode="pnp_install_failed";m.devices.leaves=true;m.devices.removable=false;
    const auto result=install(m,true,true);
    check(result.error=="pnp_install_failed"&&!result.rolledBack&&m.devices.removes==1&&m.root&&m.publisher&&text(result.certificate)=="added"&&m.written.empty()&&m.restarts==0,"trust is kept while a node of this run remains");
  }
  // ... the node is unknown: nothing more is undone or claimed.
  {
    Machine m;m.devices.installCode="pnp_install_failed";m.devices.names=false;
    const auto result=install(m,true,true);
    check(result.error=="pnp_install_failed"&&!result.rolledBack&&m.root&&m.publisher&&m.devices.removes==0,"an unknown outcome is not reported as rolled back");
  }
  // Windows bound another package to the new node: the node and the trust go.
  {
    Machine m;m.bound="3212b43e7dc20cf36046421e8fd57910d0bb5fc5e5c67c920bbfa24d6f10f95c";
    const auto result=install(m,true,true);
    check(result.error=="native_display_binding_mismatch"&&result.rolledBack&&m.devices.nodes.empty()&&!m.root&&m.written.empty()&&m.restarts==0,"a different bound package is undone");
  }
  // The selection cannot be written: the node and the trust go, the service is not restarted.
  {
    Machine m;m.selectionWritable=false;
    const auto result=install(m,true,true);
    check(result.error=="native_selection_write_failed"&&result.rolledBack&&m.devices.nodes.empty()&&!m.root&&!m.publisher&&m.restarts==0,"a failed selection write is undone");
  }
  // Windows asks for a restart: installed and selected, reported, never performed here.
  {
    Machine m;m.devices.restart=true;
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.action)=="installed"&&result.restart&&!m.written.empty()&&m.restarts==0&&m.uses==0&&m.waits==0&&text(result.service)=="unchanged","a restart request is reported with the finished install; the service is left for the Windows restart");
    check(resultJson(result).find("\"restart\":true")!=std::string::npos,"the restart flag reaches the reply");
    m.current=selected(Created);
    check(text(observe(m).state)=="restart"&&observationJson(observe(m),true,false,true,true).find("\"restartRequired\":true")!=std::string::npos,"the status keeps saying a restart is needed");
  }
  // The running service takes the device a moment later (it reads the selection
  // every second): still no restart, and nothing is said about one.
  {
    Machine m;m.adoptAfter=3;
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.service)=="adopted"&&!result.restartAttempted&&m.restarts==0&&m.waits==3&&m.waitedMs==3*AdoptionPollMs,"a service that takes the device within the wait is not restarted");
  }
  // It does not take it within the wait: restarted once, and that restart completes.
  {
    Machine m;m.adoptAfter=-1;
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.action)=="installed"&&text(result.service)=="restarted"&&result.restartAttempted&&m.restarts==1&&m.waitedMs==AdoptionWaitMs,"a service that has not taken the device is restarted after the bounded wait");
    check(m.log==std::vector<std::string>({"add:Root","add:TrustedPublisher","selection","service"})&&resultJson(result).find("\"service\":\"restarted\"")!=std::string::npos,"the restart comes after the selection");
  }
  // The restart could not be done, but the device comes up in the running
  // service afterwards: the display works, so nothing is reported.
  {
    Machine m;m.serviceRestarts=false;m.adoptAfter=int(AdoptionWaitMs/AdoptionPollMs)+2;
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.service)=="adopted"&&result.restartAttempted&&m.restarts==1&&!result.rolledBack,"a refused restart is not reported when the device is in use after all");
  }
  // Neither: the install stands, and only then the caller is told.
  {
    Machine m;m.serviceRestarts=false;m.adoptAfter=-1;
    const auto result=install(m,true,true);
    check(result.error.empty()&&text(result.action)=="installed"&&text(result.service)=="unverified"&&result.restartAttempted&&!result.rolledBack&&m.root&&m.restarts==1&&m.waitedMs==2*AdoptionWaitMs,"an unverified service does not undo the install");
    check(resultJson(result).find("\"service\":\"unverified\"")!=std::string::npos,"unverified reaches the reply");
  }
  // A state that names another node is not this device in use.
  {
    Machine m;m.adoptAfter=-1;m.serviceRestarts=false;
    struct Other final:Access {
      Machine& inner;explicit Other(Machine& machine):inner(machine){}
      std::string package()override{return inner.package();}bool certificateTrusted()override{return inner.certificateTrusted();}bool catalogTrusted()override{return inner.catalogTrusted();}
      bool addCertificate(std::vector<std::string>& added)override{return inner.addCertificate(added);}void removeCertificate(const std::string& store)override{inner.removeCertificate(store);}
      void recordTrust(const std::string& store)override{inner.recordTrust(store);}void forgetTrust(const std::string& store)override{inner.forgetTrust(store);}
      Selection selection()override{return inner.selection();}std::vector<std::wstring> recordedNodes()override{return inner.recordedNodes();}multitap::pnp::Backend& pnp()override{return inner.pnp();}std::wstring inf()override{return inner.inf();}
      std::string boundInf(const std::wstring& id)override{return inner.boundInf(id);}bool writeSelection(const std::string& json)override{return inner.writeSelection(json);}
      bool serviceUsesDevice(const std::wstring& id)override{return id!=Created;}void wait(unsigned long milliseconds)override{inner.wait(milliseconds);}bool restartService()override{return inner.restartService();}
    } access(m);
    check(text(install(access,true,true).service)=="unverified","only the selected node counts as in use");
  }
  // The durable record of what this product added to the trust stores
  // (native_display_trust.hpp), which a later removal of the Host relies on.
  {
    // Both entries added: both recorded, each only once its entry exists.
    Machine m;(void)install(m,true,true);
    check(m.trust==std::vector<std::string>({"Root","TrustedPublisher"})&&m.trustLog==std::vector<std::string>({"record:Root","record:TrustedPublisher"})&&!m.recordedWithoutEntry,"the entries this run added are recorded after they exist");
    // Trusted before Multitap (Sunshine itself is installed): nothing is recorded.
    Machine before;before.root=before.publisher=true;(void)install(before,true,true);
    check(before.trust.empty()&&before.trustLog.empty(),"a certificate that was trusted before is never recorded as Multitap's");
    // One entry was there: only the other is recorded.
    Machine half;half.root=true;(void)install(half,true,true);
    check(half.trust==std::vector<std::string>({"TrustedPublisher"})&&!half.recordedWithoutEntry,"only the entry this run added is recorded");
    // A failed run takes its entries and their record away together.
    Machine undone;undone.devices.installCode="pnp_driver_unavailable";(void)install(undone,true,true);
    check(undone.trust.empty()&&!undone.root&&!undone.publisher&&undone.trustLog==std::vector<std::string>({"record:Root","record:TrustedPublisher","forget:Root","forget:TrustedPublisher"}),"a rolled back run leaves no record");
    // The store took one entry and refused the next: the one that was added is recorded, removed and forgotten.
    struct HalfStore final:Access {
      Machine& inner;explicit HalfStore(Machine& machine):inner(machine){}
      std::string package()override{return inner.package();}bool certificateTrusted()override{return inner.certificateTrusted();}bool catalogTrusted()override{return inner.catalogTrusted();}
      bool addCertificate(std::vector<std::string>& added)override{inner.root=true;added.push_back("Root");return false;}void removeCertificate(const std::string& store)override{inner.removeCertificate(store);}
      void recordTrust(const std::string& store)override{inner.recordTrust(store);}void forgetTrust(const std::string& store)override{inner.forgetTrust(store);}
      Selection selection()override{return inner.selection();}std::vector<std::wstring> recordedNodes()override{return inner.recordedNodes();}multitap::pnp::Backend& pnp()override{return inner.pnp();}std::wstring inf()override{return inner.inf();}
      std::string boundInf(const std::wstring& id)override{return inner.boundInf(id);}bool writeSelection(const std::string& json)override{return inner.writeSelection(json);}
      bool serviceUsesDevice(const std::wstring& id)override{return inner.serviceUsesDevice(id);}void wait(unsigned long milliseconds)override{inner.wait(milliseconds);}bool restartService()override{return inner.restartService();}
    };
    Machine partial;HalfStore access(partial);const auto result=install(access,true,true);
    check(result.error=="certificate_store_failed"&&!partial.root&&partial.trust.empty()&&partial.trustLog==std::vector<std::string>({"record:Root","forget:Root"})&&!partial.recordedWithoutEntry,"a half-written store leaves neither entry nor record");
    // A node that cannot be removed keeps its publisher, and the record stays with it.
    Machine stuck;stuck.devices.installCode="pnp_install_failed";stuck.devices.leaves=true;stuck.devices.removable=false;(void)install(stuck,true,true);
    check(stuck.root&&stuck.publisher&&stuck.trust==std::vector<std::string>({"Root","TrustedPublisher"}),"trust that stays keeps its record");
    // A kept device changes neither.
    Machine kept;kept.current=selected();kept.devices.nodes[Selected]={0xA,0};(void)install(kept,true,true);
    check(kept.trustLog.empty(),"a kept device records nothing");
  }  // The numbers Host diagnostics record are fixed.
  check(errorNumber("")==0&&errorNumber("certificate_trust_required")==2&&errorNumber("pnp_install_failed")==17&&errorNumber("native_display_install_busy")==21&&errorNumber("anything else")==99,"diagnostic error numbers are append-only");
  // The stock stand-in of the setup tests behaves the same through this transaction.
  {
    Machine m;multitap::pnp::Fake standIn("absent");
    struct Wrapped final:Access {
      Machine& inner;multitap::pnp::Backend& backend;
      Wrapped(Machine& machine,multitap::pnp::Backend& stock):inner(machine),backend(stock){}
      std::string package()override{return inner.package();}bool certificateTrusted()override{return inner.certificateTrusted();}bool catalogTrusted()override{return inner.catalogTrusted();}
      bool addCertificate(std::vector<std::string>& added)override{return inner.addCertificate(added);}void removeCertificate(const std::string& store)override{inner.removeCertificate(store);}
      void recordTrust(const std::string& store)override{inner.recordTrust(store);}void forgetTrust(const std::string& store)override{inner.forgetTrust(store);}
      Selection selection()override{return inner.selection();}std::vector<std::wstring> recordedNodes()override{return inner.recordedNodes();}multitap::pnp::Backend& pnp()override{return backend;}std::wstring inf()override{return inner.inf();}
      std::string boundInf(const std::wstring& id)override{return inner.boundInf(id);}bool writeSelection(const std::string& json)override{return inner.writeSelection(json);}
      bool serviceUsesDevice(const std::wstring& id)override{return inner.serviceUsesDevice(id);}void wait(unsigned long milliseconds)override{inner.wait(milliseconds);}bool restartService()override{return inner.restartService();}
    } access(m,standIn);
    const auto result=install(access,true,true);
    check(result.error.empty()&&text(result.action)=="installed"&&standIn.installs==1&&standIn.removes==0&&result.instance==L"ROOT\\DISPLAY\\0007","setup_pnp.hpp Fake installs through the same path");
    multitap::pnp::Fake failing("install-fails");Machine n;Wrapped refused(n,failing);
    const auto failure=install(refused,true,true);
    check(failure.error=="pnp_install_failed"&&!failure.rolledBack&&failing.removes==0&&n.written.empty()&&n.restarts==0,"setup_pnp.hpp Fake failure leaves no selection and restarts nothing");
  }
  // An earlier run was closed between registering its node and writing the
  // selection: the recorded node is taken again, no second node is created.
  {
    const std::wstring Orphan=L"ROOT\\DISPLAY\\0003";
    Machine m;m.root=m.publisher=true;m.recorded={L"PCI\\VEN_1234",L"ROOT\\DISPLAY\\0009",Orphan};m.devices.nodes[Orphan]={0xA,0};
    const auto result=install(m,true,false);
    check(result.error.empty()&&text(result.action)=="reused"&&result.instance==Orphan&&result.deviceReady&&m.devices.installs==0&&m.devices.removes==0&&m.restarts==0&&text(result.service)=="adopted","a recorded unselected node is reused instead of creating another");
    check(m.written.find("\"deviceInstanceId\":\"ROOT\\\\DISPLAY\\\\0003\"")!=std::string::npos&&resultJson(result).find("\"action\":\"reused\"")!=std::string::npos,"the selection names the reused node");
  }
  // ... the certificate is still asked for when it is missing, and a reused node is never removed.
  {
    const std::wstring Orphan=L"ROOT\\DISPLAY\\0003";
    Machine m;m.recorded={Orphan};m.devices.nodes[Orphan]={0xA,0};
    check(install(m,true,false).error=="certificate_trust_required"&&!m.changed(),"reusing a node still needs the trust consent");
    m.selectionWritable=false;
    const auto result=install(m,true,true);
    check(result.error=="native_selection_write_failed"&&!result.rolledBack&&m.devices.removes==0&&m.devices.nodes.count(Orphan)==1&&m.root&&m.publisher&&m.restarts==0,"a node that was there before the run stays, with its publisher");
  }
  // ... a recorded node that is gone, or bound to another package, is not taken: a new node is created.
  {
    const std::wstring Orphan=L"ROOT\\DISPLAY\\0003";
    Machine gone;gone.recorded={Orphan};
    const auto created=install(gone,true,true);
    check(created.error.empty()&&text(created.action)=="installed"&&created.instance==Created&&gone.devices.installs==1,"a recorded node that no longer exists is not reused");
    Machine other;other.recorded={Orphan};other.devices.nodes[Orphan]={0xA,0};other.boundOf[Orphan]="3212b43e7dc20cf36046421e8fd57910d0bb5fc5e5c67c920bbfa24d6f10f95c";
    const auto beside=install(other,true,true);
    check(beside.error.empty()&&text(beside.action)=="installed"&&beside.instance==Created&&other.devices.nodes.count(Orphan)==1&&other.devices.removes==0,"a node bound to another package is left alone");
    Machine unknown;unknown.recorded={Orphan};unknown.devices.nodes[Orphan]={0xA,0};unknown.boundOf[Orphan]="";
    check(text(install(unknown,true,true).action)=="installed"&&unknown.devices.installs==1,"an unknown binding is not adopted");
  }
  // Replies carry fixed codes only.
  {
    Result leaked;leaked.error="C:\\Users\\name";
    check(resultJson(leaked).find("\"error\":\"unknown\"")!=std::string::npos&&code("pnp_install_failed")=="pnp_install_failed"&&code("Bad Code")=="unknown"&&code(std::string(65,'a'))=="unknown","free text never reaches a reply");
    check(selectionJson(L"PCI\\VEN_1234",{}).empty()&&selectionJson(L"ROOT\\DISPLAY\\00\"1",{}).empty(),"only an exact root display node is ever selected");
  }
  // The steps told to Host settings: in the order the work is done, and only what was started.
  {
    using Steps=std::vector<std::string>;
    {Machine m;install(m,true,true);check(m.steps==Steps{"display_package","display_trust","display_device","display_service"},"a first install tells its four steps in order");}
    {Machine m;install(m,false,true);check(m.steps.empty()&&!m.changed(),"an unconfirmed request told a step");}
    {Machine m;m.packageCode="native_display_package_mismatch";install(m,true,true);check(m.steps==Steps{"display_package"},"a package that is not the original went past its check");}
    {Machine m;install(m,true,false);check(m.steps==Steps{"display_package","display_trust"}&&m.devices.installs==0,"trust that was not given went on to the device");}
    {Machine m;m.current=selected();m.devices.nodes[Selected]={0x8,0};install(m,true,true);check(m.steps==Steps{"display_package"},"a kept device claimed more steps than its check");}
    // A service that does not take the device by itself is restarted, and that is said.
    {Machine m;m.adoptAfter=-1;const auto result=install(m,true,true);
     check(text(result.service)=="restarted"&&m.steps==Steps{"display_package","display_trust","display_device","display_service","display_restart"},"a restarted service was not told as a restart");}
    // A restart that could not be done is followed by one more wait for the service.
    {Machine m;m.adoptAfter=-1;m.serviceRestarts=false;const auto result=install(m,true,true);
     check(text(result.service)=="unverified"&&m.steps==Steps{"display_package","display_trust","display_device","display_service","display_restart","display_service"},"the wait after a failed restart was not told");}
    // Telling steps is not a change: the same run without anyone listening ends the same.
    {struct Silent final:Access{Machine inner;
       std::string package()override{return inner.package();}bool certificateTrusted()override{return inner.certificateTrusted();}bool catalogTrusted()override{return inner.catalogTrusted();}
       bool addCertificate(std::vector<std::string>& added)override{return inner.addCertificate(added);}void removeCertificate(const std::string& store)override{inner.removeCertificate(store);}
       void recordTrust(const std::string& store)override{inner.recordTrust(store);}void forgetTrust(const std::string& store)override{inner.forgetTrust(store);}
       Selection selection()override{return inner.selection();}std::vector<std::wstring> recordedNodes()override{return inner.recordedNodes();}multitap::pnp::Backend& pnp()override{return inner.pnp();}
       std::wstring inf()override{return inner.inf();}std::string boundInf(const std::wstring& id)override{return inner.boundInf(id);}bool writeSelection(const std::string& json)override{return inner.writeSelection(json);}
       bool serviceUsesDevice(const std::wstring& id)override{return inner.serviceUsesDevice(id);}void wait(unsigned long milliseconds)override{inner.wait(milliseconds);}bool restartService()override{return inner.restartService();}};
     Silent silent;Machine told;const auto a=install(silent,true,true),b=install(told,true,true);
     check(resultJson(a)==resultJson(b)&&silent.inner.log==told.log&&silent.inner.steps.empty()&&!told.steps.empty(),"an access that tells nothing ended differently");}
  }
  std::printf("checks=%u failed=%u\n",checks,failed);
  return failed?1:0;
}
