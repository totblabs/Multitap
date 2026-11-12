#pragma once
#include "windows_autologon.hpp"

namespace host::autologon {
// Serialized-record validity ONLY. Does not claim that current Windows account,
// Winlogon/LSA contents or streaming readiness match this preserved record.
inline bool completedJournalForUpdate(const Record& r) {
  const bool accountRequired = r.schema == 1 || r.phase == "owned";
  if ((r.schema != 1 && r.schema != 2) || !operationUuid(r.operationId) || r.intent ||
      (accountRequired && (r.account.name.empty() || r.account.domain.empty() || r.account.sid.empty())) ||
      r.account.name.size() > 256 || r.account.domain.size() > 256 || r.account.sid.size() > 256 || !baselineSafe(r.baseline) ||
      !sameSettings(r.expected, r.next) || r.expected.values[3].exists || r.expected.values[4].exists || r.expected.values[5].exists) return false;
  for (const auto* image : {&r.baseline, &r.expected, &r.next}) {
    if ((!image->secretExists && !image->secret.text.empty()) || image->secret.text.size() > 256 ||
        !image->values[5].bytes.empty() || image->plainPasswordBytes != 0) return false;
  }
  if (r.phase == "owned") return r.action == "apply" && r.expected.secretExists &&
    !r.expected.secret.text.empty() && r.expected.values[0] == textValue(L"1");
  if (r.phase == "disabled") return r.action == "disable" && baselineSafe(r.expected);
  return false;
}
} // namespace host::autologon
