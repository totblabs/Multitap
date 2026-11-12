#pragma once
// Pure Stage1 contract only. No Windows/token/process/desktop/driver calls.
// Facts below must eventually come from verified service-owned native adapters,
// never directly from a Client/worker JSON field. This model is not evidence
// that Winlogon capture/input works and is not integrated into capabilities.
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace host::signin {
using Nonce = std::array<uint8_t,32>;
using StreamId = std::array<uint8_t,16>;
template<size_t N> inline bool nonzero(const std::array<uint8_t,N>& value) { return std::any_of(value.begin(),value.end(),[](uint8_t v){return v!=0;}); }
struct Process { uint32_t pid=0;uint64_t created=0;bool operator==(const Process&)const=default; };
inline bool valid(Process p){return p.pid&&p.pid!=UINT32_MAX&&p.created;}
inline bool sid(const std::string& value){return value.size()>=7&&value.size()<=184&&value.rfind("S-1-",0)==0&&value.back()!='-'&&std::all_of(value.begin()+4,value.end(),[](char c){return (c>='0'&&c<='9')||c=='-';})&&value.find("--")==std::string::npos;}
inline bool accountSid(const std::string& value){return sid(value)&&value!="S-1-5-18"&&value!="S-1-5-19"&&value!="S-1-5-20";}
enum class Role { user, signin };
enum class Token { unknown, user, system };
enum class Desktop { unknown, normal, winlogon };
enum class WindowsState { unknown, noUser, locked, unlocked };
enum class Phase { idle, awaitLaunch, awaitBinding, awaitFrame, awaitDecoder, userActive, signinActive, awaitExit, stopping, recoveryRequired, closed };
enum class Denial { none, wrongPhase, invalidContext, notNegotiated, unauthorized, unsafeDesktop, accountMismatch, untrustedLaunch, bindingMismatch, oldWorkerAlive, invalidFrame, invalidDecoder, epochExhausted, incompleteRestore, invalidExit };
struct Result { Denial denial=Denial::none;explicit operator bool()const{return denial==Denial::none;} };
struct Lease { std::string instance;uint32_t generation=0;bool operator==(const Lease&)const=default; };
inline bool valid(const Lease& lease){constexpr const char* prefix="ROOT\\DISPLAY\\";return lease.generation&&lease.instance.size()>13&&lease.instance.size()<=21&&lease.instance.rfind(prefix,0)==0&&std::all_of(lease.instance.begin()+13,lease.instance.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');});}
struct Scope {
  uint32_t consoleSession=0;uint64_t generation=0,observedAt=0;
  WindowsState windows=WindowsState::unknown;Desktop desktop=Desktop::unknown;
  std::string signedInSid;
};
struct Context {
  StreamId stream{};Process service;bool serviceIsLocalSystem=false;
  Lease lease;bool leaseOwned=false;Nonce baseline{};
  uint32_t consoleSession=0,workerGeneration=1;uint16_t mediaEpoch=1;
  uint8_t ownedOutputMask=1;std::string configuredSid;
  Nonce nonce{};bool assigned=false,clientOptIn=false,clientTransitions=false,signinRuntimeQualified=false;
  // Explicit service admission for a freshly observed no-user console only.
  // It never permits an empty ordinary user or SYSTEM on Default desktop.
  bool unknownAccountAllowed=false;
};
struct Binding {
  StreamId stream{};Process service,worker;Lease lease;Nonce baseline{},nonce{};
  uint32_t consoleSession=0,generation=0;uint16_t mediaEpoch=0;Role role=Role::user;
};
struct Launch { Process worker;bool createdByBoundService=false,installedImageVerified=false; };
struct WorkerProof { Binding binding;Scope scope;Token token=Token::unknown;std::string tokenSid;bool privatePipePidVerified=false; };
struct FrameProof { Binding binding;Scope capturedScope;uint64_t sequence=0;uint8_t ownedOutputMask=0;bool actualDesktopPixels=false,ownedOutputsOnly=false,idr=false; };
struct DecoderAck { Binding binding;uint64_t sourceSequence=0;uint8_t ownedOutputMask=0;bool actuallyDecoded=false; };
struct InputStamp { Binding binding; };
struct ExitProof { Process process;bool waitHandleIdentityVerified=false,actuallyExited=false; };
struct RestoreProof { Process service;StreamId stream{};Lease lease;Nonce baseline{};bool fullOriginalTopologyVerified=false,consoleFallback=false,pending=true,recoveryRequired=true; };

class State {
 public:
  static constexpr uint64_t scopeMaxAgeMs=2000;
  Phase phase()const{return phase_;}
  Role role()const{return role_;}
  const Context& context()const{return context_;}
  std::optional<Process> ownedWorker()const{return owned_;}
  bool fullRestoreVerified()const{return restored_;}
  Result begin(Context context,Role role,const Scope& scope,uint64_t now){
    if(phase_!=Phase::idle)return {Denial::wrongPhase};
    if(role!=Role::user&&role!=Role::signin)return {Denial::invalidContext};
    if(!nonzero(context.stream)||!valid(context.service)||!context.serviceIsLocalSystem||!valid(context.lease)||!context.leaseOwned||!nonzero(context.baseline)||!nonzero(context.nonce)||!context.consoleSession||context.consoleSession==UINT32_MAX||!context.workerGeneration||!context.mediaEpoch||!(context.ownedOutputMask==1||context.ownedOutputMask==3)||(context.unknownAccountAllowed?!context.configuredSid.empty():!accountSid(context.configuredSid)))return {Denial::invalidContext};
    if(context.unknownAccountAllowed&&role!=Role::signin)return {Denial::accountMismatch};
    if(!context.assigned)return {Denial::unauthorized};
    if(role==Role::signin&&!negotiated(context))return {Denial::notNegotiated};
    const auto check=scopeAllowed(context,role,scope,now);if(!check)return check;
    context_=std::move(context);role_=role;scopeGeneration_=scope.generation;phase_=Phase::awaitLaunch;return {};
  }
  Result registerLaunch(const Launch& launch){
    if(phase_!=Phase::awaitLaunch)return {owned_?Denial::oldWorkerAlive:Denial::wrongPhase};
    if(!launch.createdByBoundService||!valid(launch.worker)||launch.worker.pid==context_.service.pid||lastExited_==launch.worker)return {Denial::untrustedLaunch};
    // Keep the returned owned child even when image verification fails. A
    // failed observation never frees its slot or grants a second launch.
    owned_=launch.worker;
    if(!launch.installedImageVerified){phase_=Phase::stopping;return {Denial::untrustedLaunch};}
    phase_=Phase::awaitBinding;return {};
  }
  Result workerReady(const WorkerProof& proof,uint64_t now){
    if(phase_!=Phase::awaitBinding)return {Denial::wrongPhase};
    if(!matches(proof.binding)||!proof.privatePipePidVerified)return {Denial::bindingMismatch};
    const auto check=scopeAllowed(context_,role_,proof.scope,now);if(!check)return check;
    if(proof.scope.generation!=scopeGeneration_)return {Denial::bindingMismatch};
    if(role_==Role::signin){if(proof.token!=Token::system||proof.tokenSid!="S-1-5-18")return {Denial::unsafeDesktop};}
    else if(proof.token!=Token::user||proof.tokenSid!=context_.configuredSid)return {Denial::accountMismatch};
    bound_=proof.binding;phase_=Phase::awaitFrame;return {};
  }
  Result firstFrame(const FrameProof& proof,uint64_t now){
    if(phase_!=Phase::awaitFrame)return {Denial::wrongPhase};
    if(!matches(proof.binding)||!bound_||!proof.sequence||proof.ownedOutputMask!=context_.ownedOutputMask||!proof.actualDesktopPixels||!proof.ownedOutputsOnly||!proof.idr||proof.capturedScope.generation!=scopeGeneration_||!scopeAllowed(context_,role_,proof.capturedScope,now))return {Denial::invalidFrame};
    frameSequence_=proof.sequence;phase_=Phase::awaitDecoder;return {};
  }
  Result decoderReady(const DecoderAck& ack){
    if(phase_!=Phase::awaitDecoder)return {Denial::wrongPhase};
    if(!matches(ack.binding)||!ack.actuallyDecoded||ack.sourceSequence!=frameSequence_||ack.ownedOutputMask!=context_.ownedOutputMask)return {Denial::invalidDecoder};
    phase_=role_==Role::user?Phase::userActive:Phase::signinActive;return {};
  }
  bool inputAllowed(const InputStamp& stamp,const Scope& live,uint64_t now)const{
    return context_.assigned&&active()&&matches(stamp.binding)&&bound_&&live.generation==scopeGeneration_&&bool(scopeAllowed(context_,role_,live,now));
  }
  bool redirectionAllowed(const InputStamp& stamp,const Scope& live,uint64_t now,bool assignmentPermits)const{
    return role_==Role::user&&assignmentPermits&&inputAllowed(stamp,live,now);
  }
  Result handoff(Role next,const Scope& live,const Nonce& nonce,uint64_t now){
    if(!active()||!owned_||!bound_)return {Denial::wrongPhase};
    if(next!=Role::user&&next!=Role::signin)return {Denial::invalidContext};
    if(!context_.assigned)return {Denial::unauthorized};
    // Ordinary legacy/user streaming remains valid; it gains no sign-in
    // transition authority unless both peers explicitly negotiated it.
    if(!negotiated(context_))return {Denial::notNegotiated};
    if(next==role_||!nonzero(nonce)||nonce==context_.nonce||live.generation<=scopeGeneration_)return {Denial::bindingMismatch};
    auto nextContext=context_;
    if(context_.unknownAccountAllowed){
      // Scope is a fresh native WTS observation, never a peer SID field. The
      // old SYSTEM worker still must exit before the new user worker launches.
      if(role_!=Role::signin||next!=Role::user||!accountSid(live.signedInSid))return {Denial::accountMismatch};
      nextContext.configuredSid=live.signedInSid;nextContext.unknownAccountAllowed=false;
    }
    const auto check=scopeAllowed(nextContext,next,live,now);if(!check)return check;
    if(context_.workerGeneration==UINT32_MAX||context_.mediaEpoch==UINT16_MAX)return {Denial::epochExhausted};
    context_=std::move(nextContext);++context_.workerGeneration;++context_.mediaEpoch;context_.nonce=nonce;role_=next;scopeGeneration_=live.generation;
    frameSequence_=0;bound_.reset();phase_=Phase::awaitExit;return {};
  }
  Result workerExited(const ExitProof& proof){
    if(!owned_||phase_==Phase::idle||phase_==Phase::closed)return {Denial::wrongPhase};
    if(proof.process!=*owned_||!proof.waitHandleIdentityVerified||!proof.actuallyExited)return {Denial::invalidExit};
    lastExited_=owned_;owned_.reset();bound_.reset();
    if(phase_==Phase::awaitExit)phase_=Phase::awaitLaunch;
    else{if(phase_!=Phase::recoveryRequired)phase_=Phase::stopping;finishStop();}return {};
  }
  void stop(){
    if(phase_==Phase::idle||phase_==Phase::closed)return;
    bound_.reset();frameSequence_=0;if(phase_!=Phase::recoveryRequired)phase_=Phase::stopping;finishStop();
  }
  void revoke(){context_.assigned=false;stop();}
  Result restored(const RestoreProof& proof){
    if(!(phase_==Phase::stopping||phase_==Phase::recoveryRequired))return {Denial::wrongPhase};
    if(proof.service!=context_.service||proof.stream!=context_.stream||proof.lease!=context_.lease||proof.baseline!=context_.baseline)return {Denial::bindingMismatch};
    if(!proof.fullOriginalTopologyVerified||proof.consoleFallback||proof.pending||proof.recoveryRequired){restored_=false;phase_=Phase::recoveryRequired;return {Denial::incompleteRestore};}
    // Restore can precede the worker's observed exit, as existing V2 teardown
    // restores inside the worker. Closed requires BOTH independent proofs.
    restored_=true;phase_=Phase::stopping;finishStop();return {};
  }
 private:
  Context context_;Role role_=Role::user;Phase phase_=Phase::idle;
  std::optional<Process> owned_,lastExited_;std::optional<Binding> bound_;
  uint64_t scopeGeneration_=0,frameSequence_=0;bool restored_=false;
  bool active()const{return phase_==Phase::userActive||phase_==Phase::signinActive;}
  static bool negotiated(const Context& c){return c.clientOptIn&&c.clientTransitions&&c.signinRuntimeQualified;}
  static Result scopeAllowed(const Context& c,Role role,const Scope& view,uint64_t now){
    if(view.consoleSession!=c.consoleSession||!view.generation||!view.observedAt||view.observedAt>now||now-view.observedAt>scopeMaxAgeMs)return {Denial::unsafeDesktop};
    if(role==Role::user){if(c.unknownAccountAllowed||!accountSid(c.configuredSid)||view.windows!=WindowsState::unlocked||view.desktop!=Desktop::normal)return {Denial::unsafeDesktop};if(view.signedInSid!=c.configuredSid)return {Denial::accountMismatch};}
    else{if(view.desktop!=Desktop::winlogon||!(view.windows==WindowsState::noUser||view.windows==WindowsState::locked))return {Denial::unsafeDesktop};if(view.windows==WindowsState::noUser?!view.signedInSid.empty():(c.unknownAccountAllowed||!accountSid(c.configuredSid)||view.signedInSid!=c.configuredSid))return {Denial::accountMismatch};}
    return {};
  }
  bool matches(const Binding& b)const{return owned_&&b.stream==context_.stream&&b.service==context_.service&&b.worker==*owned_&&b.lease==context_.lease&&b.baseline==context_.baseline&&b.nonce==context_.nonce&&b.consoleSession==context_.consoleSession&&b.generation==context_.workerGeneration&&b.mediaEpoch==context_.mediaEpoch&&b.role==role_;}
  void finishStop(){if(!owned_&&restored_)phase_=Phase::closed;}
};
}
