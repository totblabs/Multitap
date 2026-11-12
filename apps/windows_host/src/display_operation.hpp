#pragma once
#include "machine.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace host {
struct DisplayOperationKey {
  std::string id, nonce;
  bool matches(const Json& value) const {
    return id == value.get("operationId").string() && nonce == value.get("nonce").string();
  }
};
inline bool displayOperationUuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
    else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) return false;
  }
  return true;
}
inline DisplayOperationKey displayOperationKey(const Json& value) {
  DisplayOperationKey key{value.get("operationId").string(), value.get("nonce").string()};
  if (!displayOperationUuid(key.id) || key.nonce.size() != 43 || key.nonce.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
    throw std::runtime_error("Invalid display operation correlation");
  return key;
}
inline int displayOperationCount(const Json& value) {
  const auto count = value.get("count").number();
  if (count != 1 && count != 2) throw std::runtime_error("Invalid owned monitor count");
  return int(count);
}
inline std::string displayOperationFields(const DisplayOperationKey& key) {
  return "\"operationId\":" + jsonQuote(key.id) + ",\"nonce\":" + jsonQuote(key.nonce);
}
inline std::string safeDisplayReason(const std::string& reason) {
  for (const auto* allowed : {"invalid_display_request", "display_control_unavailable", "no_interactive_session", "operation_cancelled", "display_configuration_failed", "display_count_mismatch", "worker_failed", "display_timeout"})
    if (reason == allowed) return reason;
  return "worker_failed";
}
inline bool sameInteractiveConsole(DWORD session) {
  return session != 0 && session != 0xffffffff && WTSGetActiveConsoleSessionId() == session && interactiveSession() == session;
}

inline bool monitorControlReady(const Json& probe, const DisplayState& display, bool desktopAvailable, bool console) {
  return desktopAvailable && console && display.ready() && !display.configurationPending && !display.rebootRequired && !display.remoteSession &&
    (display.monitors.size() == 1 || display.monitors.size() == 2) && probe.get("topologySupported").boolean() && probe.get("ownedSourceCount").number() == 2;
}
}
