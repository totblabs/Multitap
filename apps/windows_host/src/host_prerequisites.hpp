#pragma once
// host-prerequisites-v1: what a Host needs besides its own program files.
//
// The Host's Windows Installer package only places files and registers the
// service. Without the rest a first install ends with a running, registered
// Host that has no virtual display device and no provider selection.
//
// This is that work as one native transaction, started by Host settings after
// the administrator's on-screen consent (settings_rpc.hpp `installNativeDisplay`)
// and never by an unattended update:
//   1. this Host's own copy of the driver package is the pinned original
//      (libvirtualdisplay 1.6.5, Sunshine-signed; the files are never changed);
//   2. the ONE pinned code-signing certificate is in LocalMachine Root and
//      TrustedPublisher (the only accepted
//      trust-store change; added only with `trustConfirmed`);
//   3. one root device node exists for it (setup_pnp.hpp);
//   4. the provider selection (ProgramData native-display.json) names that node;
//   5. the running Host service takes the device. It reads the selection every
//      second, so normally it does that by itself within a few seconds; it is
//      restarted only when it demonstrably has not, and a restart that could
//      not be done is reported only when the device is still not in use.
// A failure undoes this run's own steps: its node is removed and, only when the
// node is provably gone, the certificate entries this run added. A certificate
// or node that was there before is never removed. The selection is written last,
// so a failure before it leaves the previous selection as it was.
// Every certificate entry a run adds is recorded durably (native_display_trust.hpp),
// so that removing the Host later takes away exactly those entries and no others.
//
// This header holds every decision and is tested on fakes
// (host_prerequisites_test.cpp); host_prerequisites_native.hpp makes the calls.
#include "setup_pnp.hpp"
#include <string>
#include <vector>

namespace host::prerequisites {
// The control family the service knows (not a package version claim).
inline constexpr const char* Provider="libvirtualdisplay-1.6.4";
inline constexpr const char* DriverPackageVersion="1.6.5";
// "Sunshine Virtual Display Release Signing" (code signing only, no CA).
inline constexpr const char* PublisherThumbprint="37E100B2B382866FA7CE04EE33FE53DF53AEDC6D";

// native-display.json as it is now. `valid`: the enabled native provider with an
// exact ROOT\DISPLAY\<hex> node (what the service itself requires). Anything
// else that exists is left for an administrator, never replaced.
struct Selection {
  bool present=false,valid=false;
  std::wstring instance;
  std::string displayManager;  // "v1" / "v2" when the administrator chose one
};

class Access {
 public:
  virtual ~Access()=default;
  // "" when this Host's own package is the pinned original and its certificate
  // is the pinned publisher inside its validity period; else a fixed code.
  virtual std::string package()=0;
  // The pinned certificate is in LocalMachine Root AND TrustedPublisher.
  virtual bool certificateTrusted()=0;
  // Windows accepts the catalog's signature now, signed by the pinned publisher.
  virtual bool catalogTrusted()=0;
  // Adds the pinned certificate to each of the two stores that lacks it and
  // names the stores it added to. False: a store could not be changed.
  virtual bool addCertificate(std::vector<std::string>& added)=0;
  virtual void removeCertificate(const std::string& store)=0;
  // The durable record of the entries this product added (native_display_trust.hpp):
  // written after an entry was added, deleted when that entry is removed again.
  // Removing the Host takes away only recorded entries (host_removal.hpp); a
  // certificate that was trusted before Multitap is never recorded.
  virtual void recordTrust(const std::string& store)=0;
  virtual void forgetTrust(const std::string& store)=0;
  virtual Selection selection()=0;
  // Nodes Multitap itself registered (the ledger setup_pnp.hpp and the service
  // write: HKLM, administrators only). A record alone proves nothing more.
  virtual std::vector<std::wstring> recordedNodes()=0;
  virtual multitap::pnp::Backend& pnp()=0;
  virtual std::wstring inf()=0;
  // SHA-256 of the INF Windows bound to the node ("" when unknown).
  virtual std::string boundInf(const std::wstring& instance)=0;
  virtual bool writeSelection(const std::string& json)=0;
  // The running Host service holds this node now: its own fresh state names the
  // node with an active lease and no recovery pending.
  virtual bool serviceUsesDevice(const std::wstring& instance)=0;
  virtual void wait(unsigned long milliseconds)=0;
  // True when the Host service was observed running again as a new process.
  virtual bool restartService()=0;
};
// How long the running service gets to take a newly selected device before it
// is restarted (it normally opens the device about 2 s after the selection).
inline constexpr unsigned long AdoptionWaitMs=12000,AdoptionPollMs=500;

inline bool started(const multitap::pnp::Status& status){return (status.flags&0x8UL)!=0&&status.problem==0;}  // DN_STARTED
// cfg.h DN_NEED_RESTART (0x100) is independent of CM_PROB_NEED_RESTART (14).
inline bool restartRequired(const multitap::pnp::Status& status,bool requested){return requested||(status.flags&0x100UL)!=0||status.problem==14;}

// Only a fixed lower-case code ever reaches a reply.
inline std::string code(const std::string& value){
  if(value.size()>64)return "unknown";
  for(const char ch:value)if(!((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'))return "unknown";
  return value;
}

// The selection a new node gets: two monitors, viewport and stream-profile
// paths on, keeping an
// administrator's display manager choice.
inline std::string selectionJson(const std::wstring& instance,const Selection& previous){
  if(!multitap::pnp::validInstance(instance))return {};
  std::string id;for(const wchar_t ch:instance){if(ch==L'\\')id+="\\\\";else id+=char(ch);}
  return std::string("{\"provider\":\"")+Provider+"\",\"enabled\":true,\"deviceInstanceId\":\""+id+"\",\"monitors\":2,\"viewportTrialEnabled\":true,\"streamProfileTrialEnabled\":true,\"streamProfileTracksTrialEnabled\":true,\"driverInfSha256\":\""+
    multitap::pnp::Driver165InfSha+"\",\"driverPackageVersion\":\""+DriverPackageVersion+"\""+
    (previous.displayManager=="v1"||previous.displayManager=="v2"?",\"displayManager\":\""+previous.displayManager+"\"":std::string())+"}";
}

// ---- prerequisiteStatus (read only) -----------------------------------------
struct Observation {
  std::string package;             // "" = intact
  bool certificateTrusted=false;
  const char* selection="none";    // none | selected | invalid
  const char* device="none";       // none | ready | restart | stopped | absent | unverified
  // ready:     the selected node is started;
  // install:   nothing is selected (or the selected node is provably gone) and
  //            this Host's package can be installed;
  // restart:   the selected node waits for a Windows restart;
  // attention: something an installation here must not decide (see `reason`).
  const char* state="attention";
  std::string reason;
};
inline Observation observe(Access& access){
  Observation out;
  out.package=access.package();
  out.certificateTrusted=access.certificateTrusted();
  const auto installable=[&]{if(out.package.empty())out.state="install";else out.reason=out.package;};
  const auto selection=access.selection();
  if(!selection.present){installable();return out;}
  if(!selection.valid){out.selection="invalid";out.reason="native_selection_invalid";return out;}
  out.selection="selected";
  const auto inspection=access.pnp().inspect(selection.instance);
  if(inspection.presence=="absent"){out.device="absent";installable();return out;}
  if(inspection.presence!="present"){out.device="unverified";out.reason="native_selected_device_unverified";return out;}
  if(restartRequired(inspection.status,false)){out.device="restart";out.state="restart";return out;}
  if(started(inspection.status)){out.device="ready";out.state="ready";return out;}
  out.device="stopped";out.reason="native_display_device_not_started";return out;
}
inline std::string observationJson(const Observation& value,bool administrator,bool remoteSession,bool winfspInstalled,bool winfspPackage){
  const auto boolean=[](bool flag){return flag?"true":"false";};
  return std::string("{\"schema\":1,\"administrator\":")+boolean(administrator)+",\"remoteSession\":"+boolean(remoteSession)+
    ",\"display\":{\"state\":\""+value.state+"\",\"reason\":\""+code(value.reason)+"\",\"package\":\""+(value.package.empty()?std::string("intact"):code(value.package))+
    "\",\"certificate\":\""+(value.certificateTrusted?"trusted":"untrusted")+"\",\"selection\":\""+value.selection+"\",\"device\":\""+value.device+
    "\"},\"winfsp\":{\"installed\":"+boolean(winfspInstalled)+",\"package\":"+boolean(winfspPackage)+"},\"restartRequired\":"+boolean(std::string(value.state)=="restart")+'}';
}

// ---- installNativeDisplay ---------------------------------------------------
struct Result {
  const char* action="skipped";         // skipped | kept | installed | reused
  std::string error;
  const char* certificate="unchanged";  // unchanged | present | added | removed
  // adopted: the running service uses the device (no restart was needed, or it
  // came up although a restart could not be done); restarted: it did not, and
  // was restarted; unverified: neither; unchanged: nothing for the service to
  // do (kept, failed, or Windows must restart before the device starts).
  const char* service="unchanged";      // unchanged | adopted | restarted | unverified
  // restart: Windows asked for one (never performed). rolledBack: after a
  // failure nothing of this run is left (its node is gone, its trust removed).
  bool restart=false,deviceReady=false,rolledBack=false,restartAttempted=false;
  std::wstring instance;
};
// Append only: the index is the number Host diagnostics record (hostAction 276
// stage 1 `error`; server/admin/js/labels.js). 99: a code not listed here.
inline unsigned errorNumber(const std::string& value){
  static const char* const codes[]={"","confirmation_required","certificate_trust_required","certificate_store_failed","native_display_catalog_untrusted",
    "native_display_package_missing","native_display_package_mismatch","native_display_package_unprotected","native_display_publisher_mismatch","native_display_certificate_expired",
    "native_selection_invalid","native_selected_device_unverified","pnp_inf_invalid","pnp_unavailable","pnp_create_failed","pnp_driver_unavailable","pnp_register_failed",
    "pnp_install_failed","pnp_instance_invalid","native_display_binding_mismatch","native_selection_write_failed","native_display_install_busy"};
  for(unsigned index=0;index<sizeof(codes)/sizeof(codes[0]);++index)if(value==codes[index])return index;
  return 99;
}
inline Result install(Access& access,bool confirmed,bool trustConfirmed){
  Result out;
  const auto fail=[&](const std::string& reason){out.error=reason.empty()?std::string("unknown"):reason;return out;};
  if(!confirmed)return fail("confirmation_required");
  if(const auto package=access.package();!package.empty())return fail(package);
  const auto previous=access.selection();
  if(previous.present&&!previous.valid)return fail("native_selection_invalid");
  auto& devices=access.pnp();
  if(previous.present){
    // The protected selection names the node. Present (started or not) means it
    // is kept as it is: nothing is trusted, created, written or restarted.
    const auto inspection=devices.inspect(previous.instance);
    if(inspection.presence=="present"){
      out.action="kept";out.instance=previous.instance;out.restart=restartRequired(inspection.status,false);out.deviceReady=started(inspection.status);
      return out;
    }
    // Unknown is never permission to create another root node.
    if(inspection.presence!="absent")return fail("native_selected_device_unverified");
  }
  std::vector<std::string> added;
  // Only the entries this run added; a certificate that was there stays.
  const auto distrust=[&]{for(const auto& store:added){access.removeCertificate(store);access.forgetTrust(store);}if(!added.empty())out.certificate="removed";added.clear();};
  if(access.certificateTrusted()){
    // Trusted earlier: nothing to add, so nothing here could make Windows accept
    // a catalog it refuses (asking for consent again would change nothing).
    out.certificate="present";
    if(!access.catalogTrusted())return fail("native_display_catalog_untrusted");
  }else{
    if(!trustConfirmed)return fail("certificate_trust_required");
    // Recorded only after the entry exists: a record without its entry could
    // later claim a certificate somebody else added.
    const bool stored=access.addCertificate(added);
    for(const auto& store:added)access.recordTrust(store);
    if(!stored){distrust();return fail("certificate_store_failed");}
    out.certificate=added.empty()?"present":"added";
    if(!access.certificateTrusted()||!access.catalogTrusted()){distrust();return fail("native_display_catalog_untrusted");}
  }
  // The node goes first. Trust is withdrawn only when no node of this run can
  // still be bound to the driver (an approved publisher is kept otherwise).
  const auto undo=[&](const std::wstring& node){
    bool gone=node.empty();
    if(!gone&&multitap::pnp::validInstance(node)){
      // One removal at most: Windows may give a removed ID to the next node.
      if(devices.inspect(node).presence!="absent")devices.remove(node);
      gone=devices.inspect(node).presence=="absent";
    }
    if(gone)distrust();
    out.rolledBack=gone;
  };
  std::wstring instance;bool restart=false,created=false;
  // A run that ended between registering its node and writing the selection
  // (the process was closed) left a node nothing selects. It is taken again
  // instead of adding another one: recorded as Multitap's, still a display node
  // with exactly this driver's hardware ID (inspect), bound to the pinned INF.
  for(const auto& recorded:access.recordedNodes()){
    if(!multitap::pnp::validInstance(recorded))continue;
    if(devices.inspect(recorded).presence=="present"&&access.boundInf(recorded)==multitap::pnp::Driver165InfSha){instance=recorded;break;}
  }
  if(instance.empty()){
    const auto installed=devices.install(access.inf(),instance,restart);
    if(!installed.empty()){
      // Every code but pnp_install_failed is returned before Windows registered a
      // node. After registration the backend names the node it tried to remove;
      // without a name the outcome is unknown and nothing more is undone.
      if(installed!="pnp_install_failed"||!instance.empty())undo(instance);
      return fail(installed);
    }
    if(!multitap::pnp::validInstance(instance))return fail("pnp_instance_invalid");
    created=true;
    if(access.boundInf(instance)!=multitap::pnp::Driver165InfSha){undo(instance);return fail("native_display_binding_mismatch");}
  }
  out.instance=instance;
  const auto status=devices.status(instance);
  out.restart=restartRequired(status,restart);out.deviceReady=started(status);
  (void)devices.label(instance);  // cosmetic: a failed label never undoes a working adapter
  const auto json=selectionJson(instance,previous);
  // A node that was there before this run is never removed, and its publisher stays.
  if(json.empty()||!access.writeSelection(json)){if(created)undo(instance);return fail("native_selection_write_failed");}
  out.action=created?"installed":"reused";
  // Until Windows restarts the device cannot start, and then the service
  // starts with the selection in place: nothing to wait for or restart now.
  if(out.restart)return out;
  // Judged by the result, not by a restart. The running service reads the
  // selection every second and opens the device by itself (measured:
  // active two seconds after the selection, no restart). Only a service that
  // has not taken it within the wait is restarted, and only a restart that
  // could not be done AND a device still not in use is "unverified".
  const auto adopted=[&]{
    for(unsigned long waited=0;;waited+=AdoptionPollMs){
      if(access.serviceUsesDevice(instance))return true;
      if(waited>=AdoptionWaitMs)return false;
      access.wait(AdoptionPollMs);
    }
  };
  if(adopted()){out.service="adopted";return out;}
  out.restartAttempted=true;
  if(access.restartService()){out.service="restarted";return out;}
  out.service=adopted()?"adopted":"unverified";
  return out;
}
inline std::string resultJson(const Result& value){
  const auto boolean=[](bool flag){return flag?"true":"false";};
  return std::string("{\"action\":\"")+value.action+"\",\"error\":\""+code(value.error)+"\",\"restart\":"+boolean(value.restart)+",\"deviceReady\":"+boolean(value.deviceReady)+
    ",\"certificate\":\""+value.certificate+"\",\"service\":\""+value.service+"\",\"rolledBack\":"+boolean(value.rolledBack)+'}';
}
}  // namespace host::prerequisites
