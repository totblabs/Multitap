#pragma once
#include "machine.hpp"

namespace host {
inline std::vector<BYTE> workerTokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
  DWORD size = 0;
  GetTokenInformation(token, kind, nullptr, 0, &size);
  if (!size || size > 1024 * 1024) throw std::runtime_error("Invalid interactive token information");
  std::vector<BYTE> bytes(size);
  winCheck(GetTokenInformation(token, kind, bytes.data(), size, &size), "Interactive token information");
  return bytes;
}
template<class T> inline T workerTokenValue(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
  T value{}; DWORD size = 0;
  winCheck(GetTokenInformation(token, kind, &value, sizeof(value), &size), "Interactive token value");
  if (size != sizeof(value)) throw std::runtime_error("Invalid interactive token value");
  return value;
}
inline DWORD workerIntegrity(HANDLE token) {
  const auto bytes = workerTokenInfo(token, TokenIntegrityLevel);
  const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
  if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) throw std::runtime_error("Invalid interactive integrity SID");
  return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

// Elevation model (kept for now; a redesign is
// listed for later): for an administrator the worker runs with the linked full
// token, without a consent prompt, so that input reaches elevated windows. Its
// files in the account's own folders are therefore guarded against the
// account's medium-integrity programs: the clipboard transfer folders are
// labelled High (no write up) and everything below them is created and removed
// relative to held handles, never by a path looked up again
// (native/windows_clipboard/clipboard.cpp openBelow).
// Called by the already administrator-installed LocalSystem service. Keep the
// logged-on identity/profile/session. For an existing split-token administrator,
// use only that identity's linked elevated token so ordinary elevated application
// windows accept SendInput. Never substitute SYSTEM or another account, invent
// administrator membership, or change UAC/secure-desktop policy.
inline WinHandle selectInteractiveWorkerToken(HANDLE userToken, DWORD session) {
  if (!session || session == 0xffffffff || workerTokenValue<DWORD>(userToken, TokenSessionId) != session)
    throw std::runtime_error("Interactive worker session mismatch");
  const auto originalUser = workerTokenInfo(userToken, TokenUser);
  const auto originalSid = reinterpret_cast<const TOKEN_USER*>(originalUser.data())->User.Sid;
  if (!IsValidSid(originalSid) || IsWellKnownSid(originalSid, WinLocalSystemSid))
    throw std::runtime_error("Interactive worker requires a logged-on user");
  HANDLE selected = userToken; WinHandle linked;
  const auto elevation = workerTokenValue<TOKEN_ELEVATION_TYPE>(userToken, TokenElevationType);
  if (elevation == TokenElevationTypeLimited) {
    linked = WinHandle(workerTokenValue<TOKEN_LINKED_TOKEN>(userToken, TokenLinkedToken).LinkedToken);
    const auto linkedUser = workerTokenInfo(linked.value, TokenUser);
    const auto linkedSid = reinterpret_cast<const TOKEN_USER*>(linkedUser.data())->User.Sid;
    const DWORD integrity = workerIntegrity(linked.value);
    if (!IsValidSid(linkedSid) || !EqualSid(originalSid, linkedSid) ||
        workerTokenValue<DWORD>(linked.value, TokenSessionId) != session ||
        workerTokenValue<TOKEN_ELEVATION_TYPE>(linked.value, TokenElevationType) != TokenElevationTypeFull ||
        !workerTokenValue<TOKEN_ELEVATION>(linked.value, TokenElevation).TokenIsElevated ||
        integrity < SECURITY_MANDATORY_HIGH_RID || integrity >= SECURITY_MANDATORY_SYSTEM_RID)
      throw std::runtime_error("Linked worker token identity or integrity mismatch");
    selected = linked.value;
  }
  HANDLE primary = nullptr;
  winCheck(DuplicateTokenEx(selected, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
      TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, nullptr, SecurityImpersonation,
      TokenPrimary, &primary), "Interactive worker primary token");
  return WinHandle(primary);
}
// The private service <-> worker pipe: SYSTEM and Administrators everything,
// the worker's own Windows user read and write, nobody else. The user's entry
// names that account, not a group, so a standard user's worker opens its pipe
// exactly as an administrator's does and another account cannot
// (standard_user_access_test.cpp).
inline std::wstring workerPipeSecurityText(const std::wstring& userSid) {
  return L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + userSid + L")";
}
inline WinHandle interactiveWorkerToken(DWORD session) {
  HANDLE raw = nullptr;
  winCheck(WTSQueryUserToken(session, &raw), "Interactive user token");
  WinHandle user(raw);
  return selectInteractiveWorkerToken(user.value, session);
}
// Whether a process started with `primary` answers elevated() (machine.hpp) with
// true: the same enabled-Administrators membership test, asked of that token.
inline bool workerTokenAdministrator(HANDLE primary) {
  HANDLE raw = nullptr;
  if (!DuplicateTokenEx(primary, TOKEN_QUERY, nullptr, SecurityIdentification, TokenImpersonation, &raw)) return false;
  WinHandle impersonation(raw); BOOL admin = FALSE; SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY; PSID sid = nullptr;
  if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) return false;
  if (!CheckTokenMembership(impersonation.value, sid, &admin)) admin = FALSE;
  FreeSid(sid); return admin != FALSE;
}
// The worker the service would start in `session` (UserWorker) is an
// administrator's, so it can open the Host's protected folder itself. The
// console preparation's worker (qemu_console.hpp) still needs that: its
// restore point is read and written in place, and its pipe serves no state
// requests. Nobody signed in, or any doubt, is "no".
inline bool interactiveWorkerAdministrator(DWORD session) noexcept {
  try { const auto token = interactiveWorkerToken(session); return workerTokenAdministrator(token.value); } catch (...) { return false; }
}
}
