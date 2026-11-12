#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "signin_worker_native.hpp"
#include "display_v2.hpp"
#include "json_object.hpp"

namespace host::service_workers {class CleanupPermit;}
namespace host::signin::restoration {
inline constexpr const char* protocol="signin-private-restoration-v1";
enum class Purpose { sealedOriginalCleanup };
enum class Outcome { refused, failed, topologyRestoredOriginalHeld };
// A service-owned restore obligation, not an operator/remote stream grant.
// Parent authority must hold and observe every predecessor's exact exit.
struct RestorationAdmission {
  Purpose purpose=Purpose::sealedOriginalCleanup;
  Process service;
  StreamId stream{};
  Lease lease;
  Nonce operation{},installedImageSha256{};
  std::vector<Process> predecessors;
  std::string originalSha256;
  uint32_t consoleSession=0;
  uint64_t issuedAt=0,expiresAt=0;
};
struct RestorationBinding {
  Process service,worker;
  StreamId stream{};
  Nonce operation{};
  uint32_t consoleSession=0;
  bool operator==(const RestorationBinding&)const=default;
};
// Cleanup has no capture/input privileges. Select only from a fresh native WTS
// observation, never a peer flag, and match BOTH thread and current desktops.
enum class CleanupDesktop { winlogon,defaultDesktop };
std::optional<CleanupDesktop> cleanupDesktop(const native::ConsoleFacts&,uint32_t expectedSession,uint64_t now);
bool eligibleCleanupWorker(const RestorationBinding&,const native::ConsoleFacts&,
    const native::WorkerFacts&,uint64_t now);
struct LeaseProof {Lease lease;std::vector<NativeDisplayTarget> targets;};
struct OriginalProof {std::string bytes,sha256;};
struct Receipt {
  RestorationBinding binding;
  Outcome outcome=Outcome::refused;
  // Preserved verbatim: denying child store.remove normally leaves state-stage
  // failure even after exact topology restoration. Never rewrite it to success.
  DisplayV2Outcome underlyingOutcome=DisplayV2Outcome::failed;
  uint32_t stage=0,error=0,fallback=0;
  std::string originalSha256;
  Lease observedLease;
  uint64_t observedAt=0;
  // Canonical numeric CCD/SDK observation, not pixels or a new saved original.
  // Parent replays the semantic comparison against its held original bytes.
  std::string observedState;
};
class Authority {
 public:
  virtual ~Authority()=default;
  virtual std::optional<LeaseProof> current(const RestorationAdmission&,
      const RestorationBinding&)const=0;
  virtual std::optional<OriginalProof> original(const RestorationAdmission&,
      const RestorationBinding&)const=0;
  // Parent derives the only permitted size from its sealed original. The
  // request has no width/height/id/count fields and cannot choose a new mode.
  virtual std::optional<LeaseProof> sizeOriginal(const RestorationAdmission&,
      const RestorationBinding&)=0;
};
bool validAdmission(const RestorationAdmission&,uint64_t now);
bool sameBinding(const RestorationBinding&,const RestorationAdmission&,Process actualChild);
struct Command {std::wstring pipe;uint32_t servicePid=0;};
std::optional<Command> parseCommand(int argc,const wchar_t* const* argv);
std::string initPacket(const RestorationAdmission&,const RestorationBinding&,uint64_t serviceQuery);
std::string answer(const Json&,const RestorationAdmission&,const RestorationBinding&,
    Authority&,uint64_t& lastSerial,uint64_t now=GetTickCount64());
std::string receiptPacket(const Receipt&);
Receipt parseReceipt(const Json&);
// Pure semantic check; no Windows query, SDK call, file removal or authority.
bool originalLayoutMatches(const std::string& originalBytes,const std::string& observedState);
// Semantic/correlation/freshness check only. The parent separately proves all
// held predecessor exits, this exact restorer exit, current service/controller
// ownership and real protected-file retirement before reporting completion.
bool verifiedReceipt(const RestorationAdmission&,const RestorationBinding&,
    const OriginalProof&,const LeaseProof& actualParentLease,const Receipt&,uint64_t now);
using Exchange=std::function<Json(const Json&)>;
Receipt restoreDisplay(const RestorationAdmission&,const RestorationBinding&,
    DisplayV2Access localCcd,Exchange,std::function<uint64_t()> now);
class OwnedRestorationChild {
 public:
  OwnedRestorationChild()=default;
  OwnedRestorationChild(const OwnedRestorationChild&)=delete;
  OwnedRestorationChild& operator=(const OwnedRestorationChild&)=delete;
  ~OwnedRestorationChild();
  HANDLE process()const{return process_?process_.get():retainedProcess_.get();}
  HANDLE pipe()const{return pipe_.get();}
  const RestorationBinding& binding()const{return binding_;}
  uint64_t delegatedServiceQuery()const{return delegatedServiceQuery_;}
  bool connect(uint32_t milliseconds);
  bool stopAndWait(uint32_t milliseconds);
  native::Handle takeUnexited();
 private:
  friend native::Status launch(const RestorationAdmission&,Authority&,OwnedRestorationChild&,const service_workers::CleanupPermit*);
  native::Handle pipe_,process_,job_,imageLock_;
  service_workers::ProcessObserver retainedProcess_;
  RestorationBinding binding_;uint64_t delegatedServiceQuery_=0;
  std::wstring pipeName_;bool jobAssigned_=false;
};
native::Status launch(const RestorationAdmission&,Authority&,OwnedRestorationChild&,const service_workers::CleanupPermit* cleanupOwner=nullptr);
int workerCommand(int argc,const wchar_t* const* argv)noexcept;
} // namespace host::signin::restoration
