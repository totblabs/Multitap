#pragma once
// Distinct, not yet integrated Winlogon worker. No ordinary worker permission is
// changed here. Runtime capability stays OFF until actual capture/input/handoff
// and full display restoration are qualified by the service integration.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "service_worker_drain.hpp"
#include <windows.h>
#include "signin_stream_state.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace host::signin::native {
enum class Outcome { ready, noFrame, handoffRequired, unsupported, refused, failed };
enum class Step {
  none, admission, service, serviceImage, serviceLifetime, ownToken, privilege,
  session, account, desktop, pipe, binding, lease, createToken, createProcess,
  isolateProcess, childIdentity, connect, output, duplication, acquire, texture,
  readback, release
};
struct Status {
  Outcome outcome=Outcome::refused; Step step=Step::admission; uint32_t error=0;
  explicit operator bool() const { return outcome==Outcome::ready; }
};
class Handle {
 public:
  Handle()=default; explicit Handle(HANDLE value):value_(value){}
  Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other)noexcept:value_(other.release()){}
  Handle& operator=(Handle&& other)noexcept{if(this!=&other){reset();value_=other.release();}return *this;}
  ~Handle(){reset();}
  HANDLE get()const{return value_;}
  explicit operator bool()const{return value_&&value_!=INVALID_HANDLE_VALUE;}
  HANDLE release(){const auto value=value_;value_=nullptr;return value;}
  void reset(HANDLE value=nullptr){if(*this)CloseHandle(value_);value_=value;}
 private: HANDLE value_=nullptr;
};
// Native facts stay in process memory. No usernames, SID, pipe nonce, password,
// desktop pixels or token information are emitted through logs by this module.
struct ConsoleFacts {
  uint32_t session=0; WindowsState windows=WindowsState::unknown;
  std::string signedInSid; uint64_t observedAt=0;
  bool consoleProtocol=false, valid=false;
};
struct WorkerFacts {
  Process process; uint32_t session=0; bool localSystem=false;
  bool stationWinSta0=false, threadWinlogon=false, inputWinlogon=false;
  // Observation only for the distinct cleanup child. Capture/Permit continue
  // to require Winlogon explicitly; these flags do not grant normal SYSTEM.
  bool threadDefault=false,inputDefault=false;
};
struct OwnedTarget {
  uint32_t adapterLow=0; int32_t adapterHigh=0;
  uint32_t sourceId=0,targetId=0,width=0,height=0;
  int32_t left=0,top=0; uint8_t mask=0;
  bool operator==(const OwnedTarget&)const=default;
};
// Mandatory borrowed service authority. Root's integration must implement this
// against its existing controller/lease/baseline and authenticated session.
// It must never be populated from a peer JSON "verified" boolean. There is no
// default-success implementation and no SDK controller acquisition in this code.
class LeaseAuthority {
 public:
  virtual ~LeaseAuthority()=default;
  virtual bool current(const Binding&,std::span<const OwnedTarget>)const=0;
  // Distinct default-denied authority for the worker's OWN held-input reset.
  // Never use this capability for capture, new input or another desktop.
  virtual bool currentForRelease(const Binding&,std::span<const OwnedTarget>)const{return false;}
};
struct LaunchRequest {
  Context context; uint64_t scopeGeneration=0;
  std::array<uint8_t,32> installedImageSha256{};
};
struct NativeFailure { Status status; };

// Identity-only primitives shared with the distinct private preparer. These
// do not confer capture/input admission and never construct capture Context.
// The existing launchWorker/contextValid/Permit paths remain unchanged.
struct OwnSystemLaunchMaterial {
  std::wstring image;
  Handle imageLock, token;
};
namespace detail {
// Phase policy is only an identity check, never a cleanup/lease admission.
// Only the distinct sealed-original restorer invokes the cleanup purpose.
enum class ServiceIdentityPurpose { ordinary, sealedOriginalRestoration };
inline constexpr bool serviceIdentityPhaseAllowed(DWORD state,ServiceIdentityPurpose purpose){
  return state==SERVICE_RUNNING||
    (purpose==ServiceIdentityPurpose::sealedOriginalRestoration&&state==SERVICE_STOP_PENDING);
}
}
Status ownSystemLaunchMaterial(Process expectedService, uint32_t consoleSession,
    const Nonce& installedImageSha256, OwnSystemLaunchMaterial&);
Status verifyInstalledParentPeer(Process expectedService, HANDLE heldService,
    HANDLE actualPipe, const Nonce& installedImageSha256, Handle& imageLock);
// Recheck the actual SCM/lifetime/pipe identity while an already acquired image
// lock remains held. No token, capture, input or cleanup authority is granted.
Status verifyInstalledParentCurrent(Process expectedService,HANDLE heldService,
    HANDLE actualPipe);
// Cleanup ONLY: callers must already hold the sealed original, exact exited
// predecessors and current same-controller preparation reservation. These
// helpers additionally verify the actual SYSTEM/session-0 service token and
// admit STOP_PENDING so an existing restoration obligation can finish. They
// never grant capture/input, a new preparation, or an arbitrary service phase.
Status ownSystemRestorationLaunchMaterial(Process expectedService,uint32_t consoleSession,
    const Nonce& installedImageSha256,OwnSystemLaunchMaterial&);
Status verifyRestorationParentPeer(Process expectedService,HANDLE heldService,
    HANDLE actualPipe,const Nonce& installedImageSha256,Handle& imageLock);
Status verifyRestorationParentCurrent(Process expectedService,HANDLE heldService,
    HANDLE actualPipe);

// Real Win32 adapters. Merely linking these functions does not execute them.
Process processIdentity(HANDLE process);
ConsoleFacts observeConsole(uint32_t session);
WorkerFacts observeWorker();
Status checkConsole(const Context&,const ConsoleFacts&,uint64_t now);
Status checkWorker(const Binding&,const Context&,const ConsoleFacts&,
                   const WorkerFacts&,uint64_t now);
bool validBinding(const Binding&,const Context&);

// Own child resources survive until the service observes its exact exit. The
// integration must retain takeUnexited() if stopAndWait cannot prove exit; it
// may not release the shared display lease on destructor/pipe closure alone.
class OwnedWorker {
 public:
  OwnedWorker()=default;OwnedWorker(const OwnedWorker&)=delete;
  OwnedWorker& operator=(const OwnedWorker&)=delete;
  OwnedWorker(OwnedWorker&&)noexcept=default;
  OwnedWorker& operator=(OwnedWorker&&)=delete;
  ~OwnedWorker();
  HANDLE process()const{return process_?process_.get():retainedProcess_.get();}
  HANDLE pipe()const{return pipe_.get();}
  const Process& identity()const{return identity_;}
  const Binding& binding()const{return binding_;}
  uint64_t delegatedServiceQuery()const{return delegatedServiceQuery_;}
  const std::wstring& pipeName()const{return pipeName_;}
  bool connect(uint32_t milliseconds);
  bool stopAndWait(uint32_t milliseconds);
  Handle takeUnexited();
 private:
  friend Status launchWorker(const LaunchRequest&,OwnedWorker&);
  Handle pipe_,process_,job_;Process identity_;Binding binding_;
  service_workers::ProcessObserver retainedProcess_;
  uint64_t delegatedServiceQuery_=0;bool jobAssigned_=false;
  std::wstring pipeName_;
};
// LocalSystem service only: duplicate its OWN token, set only exact console
// SessionId, fixed WinSta0\Winlogon desktop, suspended same-image child and
// isolated job. Does not steal another process's token or change privileges.
// The caller supplies an empty owner. Once CreateProcess succeeds its handles
// stay in that owner even when a later observation fails. A failed status may
// therefore never be treated as proof that no child exists.
Status launchWorker(const LaunchRequest&,OwnedWorker&);

// Child-side exact binding. The serviceQuery handle is already delegated by
// the service into this owned child through the existing private PID-verified
// transport. The caller retains it for the complete Permit/Capture lifetime.
class Permit {
 public:
  Permit(const Permit&)=delete;Permit& operator=(const Permit&)=delete;
  const Binding& binding()const{return binding_;}
  std::span<const OwnedTarget> targets()const{return targets_;}
  Status current()const;
  Status currentForRelease()const;
 private:
  friend std::unique_ptr<Permit> authorizeWorker(const Binding&,const Context&,
      HANDLE,HANDLE,std::span<const OwnedTarget>,const LeaseAuthority&,Status&);
  Permit()=default;
  Status checkCurrent(bool releaseOnly)const;
  Binding binding_;Context context_;HANDLE service_=nullptr,pipe_=nullptr;
  std::vector<OwnedTarget> targets_;const LeaseAuthority* authority_=nullptr;
};
std::unique_ptr<Permit> authorizeWorker(const Binding&,const Context&,
    HANDLE serviceQuery,HANDLE authenticatedPipe,std::span<const OwnedTarget>,
    const LeaseAuthority&,Status&);
}
