#pragma once
#include <atomic>
#include <cstdint>
namespace host {
// Only the authenticated broker's explicit protocol ACK enables telemetry.
// Receiver-thread requests survive the sender's five-second rate limit.
class HostDiagnosticSchedule {
 public:
  void Reset(){allowed_=false;requested_=false;sentAt_=-1;}
  void Negotiate(bool supported){const bool previous=allowed_.exchange(supported);if(supported&&!previous)requested_=true;if(!supported)requested_=false;}
  void Request(){if(allowed_)requested_=true;}
  bool Claim(int64_t now){
    if(now<0||!allowed_)return false;
    const auto prior=sentAt_.load();
    if(prior>=0&&(now<prior||now-prior<5000))return false;
    if(!requested_&&prior>=0&&now-prior<30000)return false;
    requested_=false;sentAt_=now;return allowed_;
  }
 private:
  std::atomic<bool> allowed_{false},requested_{false};
  std::atomic<int64_t> sentAt_{-1};
};
}
