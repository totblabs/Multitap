#pragma once
// Display labels may change; existing queue ports, drivers and record paths do not.
#include "machine.hpp"
#include <string>

namespace host {
// "(PC)", not "(내 PC)" (product wording: PC, never 이/내 PC). Queues and records of the previous label stay
// recognised as the legacy name (removed by ownership, v2 default records restored).
inline const std::wstring printQueueName = L"Multitap 프린터 (PC)";
inline const std::wstring printLegacyQueueName = L"Multitap 프린터 (내 PC)";
inline constexpr const wchar_t* printPortPrefix = L"Multitap_Print_";
inline constexpr const wchar_t* printDriverName = L"Microsoft Print To PDF";
inline constexpr const wchar_t* printMonitorName = L"Standard TCP/IP Port";

inline bool samePrintText(const std::wstring& a, const std::wstring& b) {
  return a.size() == b.size() && (a.empty() || CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL);
}
inline bool printOwnedPort(const std::wstring& port) {
  const std::wstring prefix(printPortPrefix);
  if (port.size() <= prefix.size() || port.size() > prefix.size() + 5 || !samePrintText(port.substr(0, prefix.size()), prefix)) return false;
  unsigned value = 0;
  for (size_t at = prefix.size(); at < port.size(); ++at) {
    if (port[at] < L'0' || port[at] > L'9') return false;
    value = value * 10 + unsigned(port[at] - L'0');
  }
  return value > 0 && value <= 65535 && port.substr(prefix.size()) == std::to_wstring(value);
}
struct PrintQueueBinding { std::wstring port, driver; };
inline bool printOwnedBinding(const PrintQueueBinding& binding) {
  // This is the product's existing reserved-port plus inbox-driver evidence,
  // not cryptographic proof of who created a legacy queue.
  return printOwnedPort(binding.port) && samePrintText(binding.driver, printDriverName);
}
}  // namespace host
