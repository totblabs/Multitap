#pragma once
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace host {
class ClipboardPairGeneration {
 public:
  static constexpr uint64_t maximum=9007199254740991ULL;
  bool advance(){const auto old=value_.load();if(old>=maximum)return false;value_=old+1;return true;}
  uint64_t current()const{return value_.load();}
  bool accepts(uint64_t expected)const{return expected!=0&&expected==value_.load();}
 private:
  std::atomic<uint64_t> value_{0};
};
inline uint64_t clipboardGenerationNumber(double value){
  return std::isfinite(value)&&value>0&&value<=double(ClipboardPairGeneration::maximum)&&std::floor(value)==value?uint64_t(value):0;
}
// One streaming session gets at most three fresh clipboard pairs. Never resume
// a transfer, relax authorization, or retry a protocol rejection.
//
// host-resume-v1 (host_resume.hpp): while the stream itself is held through a
// lost broker channel, the clipboard channel is lost with it, and a failed dial
// says nothing about the channel. Such a failure costs no attempt (the three
// used to be spent in about 7 s of a longer outage, and the resumed stream kept
// a dead clipboard). The channel waits instead, and the resumed stream gives
// it one fresh set of the same three. A channel whose three were spent before
// the hold was noticed waits the same way. A refusal or a protocol failure
// still ends it for good, away or not, and is never brought back by a resume.
// Not thread-safe: ClipboardChannel calls it under its mutex.
class ClipboardReconnect {
 public:
  enum class Step { retry, wait, stop };
  static constexpr unsigned maximumAttempts=3;
  static bool transientReason(const std::string& reason) {
    // clipboard_stream_away: channel-resume-v1 (channel_redial.hpp), the Broker closed the pair because the client or this Host is away.
    return reason=="clipboard_transport_closed"||reason=="clipboard_pair_timeout"||reason=="clipboard_timeout"||reason=="clipboard_stream_away";
  }
  // The stream is held: its broker channel is away.
  void away(){away_=true;}
  // The held stream continues. True: a waiting channel dials again (ask next()).
  bool resumed(){
    away_=false;
    if(!waiting_)return false;
    waiting_=false;attempts_=0;return true;
  }
  bool waiting()const{return waiting_;}
  bool isAway()const{return away_;}
  // After a pair ended. retry: dial again after `delay`. wait: until resumed().
  // stop: never again in this session.
  Step next(bool transient,std::chrono::milliseconds& delay) {
    if(!transient)return Step::stop;
    if(away_||attempts_>=maximumAttempts){waiting_=true;return Step::wait;}
    delay=std::chrono::milliseconds(1000u<<attempts_++);return Step::retry;
  }
 private:
  unsigned attempts_=0;
  bool away_=false,waiting_=false;
};
}
