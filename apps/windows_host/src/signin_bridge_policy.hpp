#pragma once
#include "signin_stream_state.hpp"
#include <string_view>
#include <functional>
#include <mutex>
#include <vector>

namespace host::signin::broker {
inline constexpr std::string_view protocol="signin-broker-admission-v1";
using PhaseId = std::array<uint8_t,16>;
struct Preference { bool enabled=false; };
// Discovery withdrawal never revokes an active streaming authority. The native
// producer serializes this state with its existing monotonic revision counter.
class DiscoveryPublication {
 public:
  enum class Action { offer,withdraw };
  std::optional<Action> observe(bool eligible){if(eligible){published_=true;return Action::offer;}if(!published_)return {};published_=false;return Action::withdraw;}
 private:bool published_=false;
};
// Synchronous retirement is stronger than an auth flag: once retire returns,
// no retained worker callback can enter a destroyed Agent/connection owner.
class OutputLease {
 public:
  using Callback=std::function<void(bool,std::vector<uint8_t>)>;
  explicit OutputLease(Callback callback):callback_(std::move(callback)){}
  bool send(bool text,std::vector<uint8_t> bytes){std::lock_guard lock(mutex_);if(!callback_)return false;callback_(text,std::move(bytes));return true;}
  void retire(){std::lock_guard lock(mutex_);callback_={};}
 private:std::mutex mutex_;Callback callback_;
};
struct Admission {
  StreamId stream{}; PhaseId transition{}; Process service;
  uint32_t workerGeneration=0,grantRevision=0,eligibilityRevision=0;
  uint16_t mediaEpoch=0;uint64_t receivedAt=0,validForMs=0;
  std::string serviceInstance;
};
struct Observation {
  Process service;uint32_t console=0;uint64_t generation=0,at=0;
  WindowsState windows=WindowsState::unknown;std::string sid;
  bool system=false,consoleProtocol=false,ownedSingleOutput=false;
};
inline bool fresh(const Observation& o,uint64_t now){return valid(o.service)&&o.system&&o.console&&o.console!=UINT32_MAX&&o.consoleProtocol&&o.ownedSingleOutput&&o.generation&&o.at&&o.at<=now&&now-o.at<=1000;}
inline bool ordinaryUserSid(const std::string& value){return sid(value)&&value!="S-1-5-18"&&value!="S-1-5-19"&&value!="S-1-5-20";}
inline bool admissionCurrent(const Admission& a,const Preference& p,bool compiledBackend,
    const std::string& instance,uint32_t revision,const Observation& o,uint64_t now){
  return p.enabled&&compiledBackend&&fresh(o,now)&&a.service==o.service&&nonzero(a.stream)&&nonzero(a.transition)&&
    a.workerGeneration&&a.mediaEpoch&&a.grantRevision&&a.eligibilityRevision==revision&&
    !instance.empty()&&a.serviceInstance==instance&&a.receivedAt&&a.receivedAt<=now&&
    a.validForMs&&a.validForMs<=1000&&now-a.receivedAt<a.validForMs&&
    ((o.windows==WindowsState::noUser&&o.sid.empty())||(o.windows==WindowsState::locked&&ordinaryUserSid(o.sid)));
}
// The production service supplies native observations and held-process exit
// facts. These methods do not turn a peer boolean into Windows authority.
class PhaseFence {
 public:
  enum class Phase { idle,preparing,frame,decoded,active,retiring,closed };
  bool begin(const Admission& a,const Observation& o){
    if(phase_!=Phase::idle||!a.workerGeneration||!a.mediaEpoch||!nonzero(a.transition)||!o.console||!o.generation)return false;
    admission_=a;observation_=o;role_=Role::signin;phase_=Phase::preparing;return true;
  }
  bool worker(Process child){if(phase_!=Phase::preparing||owned_||!valid(child)||child==admission_.service)return false;owned_=child;phase_=Phase::frame;return true;}
  bool firstFrame(uint64_t sequence,const PhaseId& id){if(phase_!=Phase::frame||!owned_||!sequence||!nonzero(id))return false;sequence_=sequence;frame_=id;phase_=Phase::decoded;return true;}
  bool decoded(uint32_t generation,uint16_t epoch,const PhaseId& transition,const PhaseId& frame){
    if(phase_!=Phase::decoded||!matches(generation,epoch,transition)||frame!=frame_)return false;phase_=Phase::active;return true;
  }
  bool input(uint32_t generation,uint16_t epoch,const PhaseId& transition,const Observation& live,uint64_t now)const{
    if(phase_!=Phase::active||!matches(generation,epoch,transition)||!fresh(live,now)||live.service!=observation_.service||live.console!=observation_.console||live.generation!=observation_.generation)return false;
    return role_==Role::signin?live.windows==observation_.windows&&live.sid==observation_.sid:live.windows==WindowsState::unlocked&&live.sid==observation_.sid;
  }
  bool transition(Role next,const Observation& live,const PhaseId& nonce,uint64_t now){
    if((phase_!=Phase::active&&phase_!=Phase::decoded&&phase_!=Phase::frame)||next==role_||!fresh(live,now)||live.service!=observation_.service||live.console!=observation_.console||!nonzero(nonce)||nonce==admission_.transition||admission_.workerGeneration==UINT32_MAX||admission_.mediaEpoch==UINT16_MAX)return false;
    if(next==Role::user){if(live.windows!=WindowsState::unlocked||!ordinaryUserSid(live.sid))return false;if(!observation_.sid.empty()&&live.sid!=observation_.sid)return false;}
    else if(!((live.windows==WindowsState::locked&&live.sid==observation_.sid)||(live.windows==WindowsState::noUser&&live.sid.empty())))return false;
    released_=role_!=Role::signin;
    ++admission_.workerGeneration;++admission_.mediaEpoch;admission_.transition=nonce;observation_=live;role_=next;
    phase_=Phase::retiring;frame_={};sequence_=0;exited_=restored_=false;return true;
  }
  void revoke(){phase_=Phase::retiring;frame_={};sequence_=0;}
  bool exited(Process child,bool verified){if(phase_!=Phase::retiring||!owned_||child!=*owned_||!verified)return false;exited_=true;return true;}
  bool released(Process child,bool verified){if(phase_!=Phase::retiring||!owned_||child!=*owned_||!verified)return false;released_=true;return true;}
  bool restored(bool verified){if(phase_!=Phase::retiring||!verified)return false;restored_=true;return true;}
  bool successor(){if(phase_!=Phase::retiring||!exited_||!restored_||!released_)return false;owned_.reset();phase_=Phase::preparing;return true;}
  bool matches(uint32_t generation,uint16_t epoch,const PhaseId& transition)const{return admission_.workerGeneration==generation&&admission_.mediaEpoch==epoch&&admission_.transition==transition;}
  const Admission& admission()const{return admission_;}const Observation& observation()const{return observation_;}
  Role role()const{return role_;}Phase phase()const{return phase_;}std::optional<Process> owned()const{return owned_;}
 private:
  Admission admission_;Observation observation_;Role role_=Role::signin;Phase phase_=Phase::idle;
  std::optional<Process> owned_;PhaseId frame_{};uint64_t sequence_=0;bool exited_=false,restored_=false,released_=false;
};
}
