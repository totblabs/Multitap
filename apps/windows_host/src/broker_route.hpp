#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
// host-route-v1 (Broker MULTITAP_HOST_ROUTE, sent in host.capabilities.ack): the Broker's
// address inside this network. A router that loops traffic for its own public address
// back (hairpin NAT) stalled the Host channel 80-90 ms every 2.1 s in the field;
// this skips it. WinHTTP still sends and checks the public name (SNI,
// certificate), so only the address lookup changes. After a routed attempt fails, the
// next ones use DNS for ten minutes.
namespace host {
inline constexpr uint32_t brokerRouteAction = 272;  // stage 1 connected through the route, 2 failed there
// A dotted IPv4 literal, or nothing.
inline std::optional<std::wstring> brokerRouteAddress(const std::string& value) {
  if (value.empty() || value.size() > 15) return std::nullopt;
  int parts = 0, digits = 0, octet = 0;
  for (const char ch : value) {
    if (ch == '.') { if (!digits || ++parts > 3) return std::nullopt; digits = octet = 0; continue; }
    if (ch < '0' || ch > '9' || (digits == 1 && octet == 0)) return std::nullopt;
    octet = octet * 10 + (ch - '0'); if (++digits > 3 || octet > 255) return std::nullopt;
  }
  if (parts != 3 || !digits) return std::nullopt;
  return std::wstring(value.begin(), value.end());
}
class BrokerRoute {
 public:
  static constexpr uint64_t coolDownMs = 600000;
  // The Broker's offer; anything but an IPv4 literal clears the route. True when it changed.
  bool offer(const std::string& value) {
    const auto next = brokerRouteAddress(value).value_or(std::wstring());
    std::lock_guard lock(mutex_);
    if (next == route_) return false;
    route_ = next; failed_ = false;
    return true;
  }
  std::wstring current(uint64_t nowMs) const {
    std::lock_guard lock(mutex_);
    return failed_ && nowMs - failedAt_ < coolDownMs ? std::wstring() : route_;
  }
  void failed(uint64_t nowMs) { std::lock_guard lock(mutex_); failed_ = true; failedAt_ = nowMs; }
 private:
  mutable std::mutex mutex_;
  std::wstring route_;
  bool failed_ = false;
  uint64_t failedAt_ = 0;
};
inline BrokerRoute& brokerRoute() { static BrokerRoute route; return route; }
}
