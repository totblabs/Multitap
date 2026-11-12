#pragma once
// A first password for the shared Windows account: Host
// settings sets one on a local account that has none instead of sending the
// user to Windows Settings. It never resets an existing password, answers only
// in fixed codes and never logs or keeps the password (the account save that
// follows stores it, encrypted, as before).
#include "windows_account_record.hpp"
#include "windows_autologon_native.hpp"

namespace host::account_password {
using autologon::Account;
using autologon::Failure;
using autologon::Secret;
// Every Windows call; QA substitutes fakes (settings_rpc_test.cpp).
struct Access {
  std::function<Account(const std::wstring&)> account;   // local accounts only (autologon::localAccount)
  std::function<DWORD(const Account&, const Secret&)> logon;  // LogonUserW: only the Win32 result
  // Mutation must compare the empty OLD password inside Windows, never reset.
  std::function<DWORD(const Account&, const Secret&)> set;
  std::function<uint32_t()> minimumLength;               // NetUserModalsGet level 0; 0 when unread
  std::function<std::wstring()> random;                  // a password nobody has
};
enum class Current { empty, set, unknown };
// Windows refuses an empty password with the blank-password restriction
// (LimitBlankPasswordUse) only after it matched; a random password refused as
// wrong proves the restriction did not come before the password check.
inline Current currentPassword(const Access& access, const Account& account) {
  const auto wrong = [](DWORD code) { return code == ERROR_LOGON_FAILURE || code == ERROR_WRONG_PASSWORD; };
  const DWORD blank = access.logon(account, Secret{});
  if (blank == ERROR_SUCCESS) return Current::empty;
  if (wrong(blank)) return Current::set;
  if (blank != ERROR_ACCOUNT_RESTRICTION) return Current::unknown;
  return wrong(access.logon(account, Secret{access.random()})) ? Current::empty : Current::unknown;
}
inline const char* accountCode(const char* code) {
  for (const char* known : {"local_account_required", "account_invalid", "account_unavailable"})
    if (std::string_view(code) == known) return known;
  return "account_unavailable";
}
inline const char* setCode(DWORD result) {
  switch (result) {
    case NERR_Success: return "";
    case NERR_PasswordTooShort: case NERR_PasswordTooLong: case NERR_PasswordNotComplexEnough: case NERR_PasswordFilterError:
    case NERR_PasswordHistConflict: case NERR_PasswordTooRecent: case ERROR_PASSWORD_RESTRICTION: case ERROR_ILL_FORMED_PASSWORD:
      return "password_policy";
    case ERROR_ACCESS_DENIED: return "password_set_denied";
    case ERROR_INVALID_PASSWORD: case ERROR_WRONG_PASSWORD: case ERROR_LOGON_FAILURE:
      return "password_exists";
    case NERR_UserNotFound: return "account_changed";
    default: return "password_set_failed";
  }
}
inline std::string codeJson(const char* code) { return std::string("{\"error\":\"") + code + "\"}"; }
// windowsAccountPasswordState: whether the account the user typed is local and
// has no password, with its SID (the set request must name the same one) and
// the minimum length Windows asks for.
inline std::string passwordStateResult(const Access& access, const Json& request) {
  try {
    const auto account = access.account(wide(request.get("username").string()));
    if (!account.usable) return codeJson("account_restricted");
    const auto current = currentPassword(access, account);
    return "{\"error\":\"\",\"state\":\"" + std::string(current == Current::empty ? "empty" : current == Current::set ? "set" : "unknown") +
      "\",\"sid\":" + jsonQuote(utf8(account.sid)) + ",\"minimumLength\":" + std::to_string(std::min<uint32_t>(access.minimumLength(), 256)) + '}';
  } catch (const Failure& failure) { return codeJson(accountCode(failure.what())); }
  catch (...) { return codeJson("account_unavailable"); }
}
// setWindowsAccountPassword: confirmed, both entries equal, the same local
// account (SID) the state check showed, still without a password, and once
// more the same SID right before the OS-enforced change from an empty password.
inline std::string setPasswordResult(const Access& access, const Json& request) {
  const auto* object = std::get_if<Json::Object>(&request.value);
  if (!object) return codeJson("invalid_request");
  for (const auto& field : *object)
    if (field.first != "action" && field.first != "username" && field.first != "sid" && field.first != "password" && field.first != "passwordConfirmation" && field.first != "confirmed")
      return codeJson("invalid_request");
  const auto* confirmed = std::get_if<bool>(&request.get("confirmed").value);
  if (!confirmed || !*confirmed) return codeJson("confirmation_required");
  const auto* password = std::get_if<std::string>(&request.get("password").value);
  const auto* confirmation = std::get_if<std::string>(&request.get("passwordConfirmation").value);
  if (!password || !confirmation || *password != *confirmation) return codeJson("password_confirmation");
  Secret secret;
  try { secret.text = wide(*password); } catch (...) { return codeJson("password_invalid_input"); }
  if (!windowsAccountText(secret.text, 1, 256)) return codeJson("password_invalid_input");
  const auto sid = request.get("sid").string();
  try {
    const auto name = wide(request.get("username").string());
    const auto account = access.account(name);
    if (sid.empty() || utf8(account.sid) != sid) return codeJson("account_changed");
    if (!account.usable) return codeJson("account_restricted");
    const auto current = currentPassword(access, account);
    if (current == Current::set) return codeJson("password_exists");
    if (current == Current::unknown) return codeJson("password_state_unknown");
    const auto latest = access.account(name);
    if (latest.sid != account.sid || _wcsicmp(latest.name.c_str(), account.name.c_str()) != 0 ||
        _wcsicmp(latest.domain.c_str(), account.domain.c_str()) != 0) return codeJson("account_changed");
    if (!latest.usable) return codeJson("account_restricted");
    return codeJson(setCode(access.set(account, secret)));
  } catch (const Failure& failure) { return codeJson(accountCode(failure.what())); }
  catch (...) { return codeJson("password_set_failed"); }
}
inline DWORD logonLocal(const Account& account, const Secret& password) {
  HANDLE token = nullptr;
  if (!LogonUserW(account.name.c_str(), account.domain.c_str(), password.text.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &token)) return GetLastError();
  CloseHandle(token); return ERROR_SUCCESS;
}
// Only this adapter invokes the native mutation. QA injects both the account
// lookup and primitive; it never invokes nativeAccess()'s real mutation.
inline DWORD changeLocalFromEmpty(const Account& account, const Secret& password,
    const autologon::AccountLookup& lookup = {},
    decltype(&::NetUserChangePassword) change = &::NetUserChangePassword) {
  const auto machine = lookup.machine();
  if (!change || machine.empty() || machine.size() > MAX_COMPUTERNAME_LENGTH ||
      machine.find_first_of(L"\\/@:.") != std::wstring::npos ||
      std::any_of(machine.begin(), machine.end(), [](wchar_t c) { return c < 32; }) ||
      account.name.empty() || account.sid.empty() || !account.usable ||
      _wcsicmp(account.domain.c_str(), machine.c_str()) != 0) return ERROR_INVALID_PARAMETER;
  const auto current = autologon::localAccount(account.name, lookup);
  if (current.sid != account.sid || _wcsicmp(current.name.c_str(), account.name.c_str()) != 0 ||
      _wcsicmp(current.domain.c_str(), machine.c_str()) != 0 || !current.usable ||
      _wcsicmp(lookup.machine().c_str(), machine.c_str()) != 0) return NERR_UserNotFound;
  // Explicit LOCAL server; NULL would select the caller's logon domain.
  const std::wstring server = L"\\\\" + machine;
  // NetAPI names users, not SIDs. The final lookup cannot atomically exclude
  // a later same-name account replacement. This is not a SID-bound CAS.
  // It DOES atomically refuse a password created since the empty-state read.
  return DWORD(change(server.c_str(), current.name.c_str(), L"", password.text.c_str()));
}
inline Access nativeAccess() {
  Access access;
  access.account = [](const std::wstring& name) { return autologon::localAccount(name); };
  access.logon = logonLocal;
  access.set = [](const Account& account, const Secret& password) {
    return changeLocalFromEmpty(account, password);
  };
  access.minimumLength = [] {
    LPBYTE raw = nullptr; if (NetUserModalsGet(nullptr, 0, &raw) != NERR_Success || !raw) return 0u;
    const uint32_t length = reinterpret_cast<USER_MODALS_INFO_0*>(raw)->usrmod0_min_passwd_len; NetApiBufferFree(raw); return length;
  };
  access.random = [] { return randomName() + L"#Aa1"; };
  return access;
}
}  // namespace host::account_password
