#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

namespace multitap {
// Process-local publication permission, shared by a window and all its queued
// writes. This is not a durable tombstone or proof of the Windows user/desktop.
class ThumbnailGeneration {
 public:
  struct State {
    std::atomic<uint64_t> stamp{0},durablyRevoked{0};
    // Sign-in windows never publish, including after a decoder restart or
    // role handoff. Only destroying this state and creating a new window
    // creates an unlocked owner. A queued callback cannot clear this latch.
    std::atomic<bool> privacyLocked{false};
  };
  struct Ticket { std::shared_ptr<State> state;uint64_t stamp=0; };
  static Ticket Own(){
    auto state=std::make_shared<State>();const auto stamp=Next(true);
    state->stamp=stamp;return {std::move(state),stamp};
  }
  static bool Current(const Ticket& ticket,bool allowed=true){
    return ticket.state&&ticket.stamp&&(!allowed||!ticket.state->privacyLocked.load())&&
      bool(ticket.stamp&1)==allowed&&ticket.state->stamp.load()==ticket.stamp;
  }
  // This closes in-memory publication immediately. The caller must still use
  // ThumbnailWriter::Invalidate and await its durable purge acknowledgement.
  static bool Lock(const Ticket& ticket){
    if(!ticket.state||!ticket.stamp||ticket.state->stamp.load()!=ticket.stamp)return false;
    ticket.state->privacyLocked=true;return true;
  }
  static bool Locked(const Ticket& ticket){return ticket.state&&ticket.state->privacyLocked.load();}
  // CAS prevents a late old owner from reviving or revoking its replacement.
  static Ticket Change(const Ticket& old,bool allowed){
    if(!old.state||!old.stamp)return {};
    if(allowed&&old.state->privacyLocked.load()){
      // Preserve an acknowledged revoke through decoder restarts. Creating a
      // fresh stamp here would unnecessarily invalidate its durable marker.
      if(Current(old,false))return old;
      allowed=false;
    }
    auto expected=old.stamp;const auto next=Next(allowed);
    if(!next||!old.state->stamp.compare_exchange_strong(expected,next))return {};
    return {old.state,next};
  }
  static void Retire(const std::shared_ptr<State>& state){if(state)state->stamp=0;}
  static bool DurablyRevoked(const Ticket& ticket){return Current(ticket,false)&&ticket.state->durablyRevoked.load()==ticket.stamp;}
  // Used by the real final-rename seam. If revoke races the already-entered
  // operation, the same owner discards its exact result before returning.
  template<class Commit,class Discard>
  static bool Publish(const Ticket& ticket,Commit commit,Discard discard){
    if(!Current(ticket)||!commit())return false;
    if(Current(ticket))return true;
    discard();return false;
  }
 private:
  static uint64_t Next(bool allowed){
    static std::atomic<uint64_t> serial{0};auto current=serial.load();
    for(;;){if(current>=uint64_t(std::numeric_limits<int64_t>::max())/2-1)return 0;
      if(serial.compare_exchange_weak(current,current+1))return ((current+1)<<1)|(allowed?1u:0u);}
  }
};
}
