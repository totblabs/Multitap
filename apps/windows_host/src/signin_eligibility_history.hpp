#pragma once
#include "signin_bridge_policy.hpp"
#include <deque>
#include <algorithm>

namespace host::signin::broker {
struct EligibilityScope {
  Observation native;
  std::string lease,configuration;
  uint64_t connectionGeneration=0;
  bool valid(uint64_t now)const{
    return fresh(native,now)&&connectionGeneration&&!lease.empty()&&!configuration.empty()&&
      ((native.windows==WindowsState::locked&&ordinaryUserSid(native.sid))||(native.windows==WindowsState::noUser&&native.sid.empty()));
  }
  bool same(const EligibilityScope& b)const{
    return native.service==b.native.service&&native.console==b.native.console&&native.generation==b.native.generation&&
      native.windows==b.native.windows&&native.sid==b.native.sid&&native.system==b.native.system&&
      native.consoleProtocol==b.native.consoleProtocol&&native.ownedSingleOutput==b.native.ownedSingleOutput&&
      lease==b.lease&&configuration==b.configuration&&connectionGeneration==b.connectionGeneration;
  }
};
// Only the native producer records entries, after actual current-WSS and native
// scope observation. Peer revision numbers never create or renew an entry.
class EligibilityHistory {
 public:
  bool published(uint32_t revision,const EligibilityScope& scope,uint64_t now){
    if(!time(now)||!revision||revision<=lastRevision_||!scope.valid(now))return false;
    if(!scope_||!scope_->same(scope))clear();
    scope_=scope;lastRevision_=revision;prune(now);entries_.push_back({revision,now});
    while(entries_.size()>32)entries_.pop_front();return true;
  }
  bool accepts(uint32_t revision,const EligibilityScope& current,uint64_t now){
    if(!time(now)||!scope_||!current.valid(now)||!scope_->same(current)){clear();return false;}
    prune(now);return std::any_of(entries_.begin(),entries_.end(),[&](const Entry& e){return e.revision==revision;});
  }
  void clear(){entries_.clear();scope_.reset();}
 private:
  struct Entry{uint32_t revision;uint64_t at;};
  bool time(uint64_t now){if(poisoned_||!now||now<lastNow_){poisoned_=true;clear();return false;}lastNow_=now;return true;}
  void prune(uint64_t now){while(!entries_.empty()&&now-entries_.front().at>=5000)entries_.pop_front();}
  std::deque<Entry> entries_;std::optional<EligibilityScope> scope_;uint32_t lastRevision_=0;uint64_t lastNow_=0;bool poisoned_=false;
};
struct DesktopPacketSource {
  uint32_t generation=0;PhaseId transition{};
  bool current(const Admission& a)const{return generation&&nonzero(transition)&&generation==a.workerGeneration&&transition==a.transition;}
};
}
