// The Host service's own list (service_control_security.hpp): after the service
// starts, only SYSTEM and Administrators may send its user-defined controls 128
// (reload) and 129 (capabilities). Checked on descriptors in text with a real
// AccessCheck against a stand-in token with only Everyone / Users / Authenticated
// Users / INTERACTIVE enabled (no administrator, no SYSTEM). No service is
// opened or changed.
#include "service_control_security.hpp"
#include <sddl.h>
#include <cstdio>
#include <string>
#include <stdexcept>
#pragma comment(lib, "advapi32.lib")
namespace {
unsigned checks = 0, failed = 0;
void check(bool value, const char* name) { ++checks; if (!value) { ++failed; std::printf("FAIL %s\n", name); } }
struct Descriptor { PSECURITY_DESCRIPTOR value = nullptr; ~Descriptor() { if (value) LocalFree(value); } };
const GENERIC_MAPPING serviceMapping{SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS | READ_CONTROL,
  SERVICE_CHANGE_CONFIG | READ_CONTROL, SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL | READ_CONTROL, SERVICE_ALL_ACCESS};
// The list after the service applied its rule, as text (for the AccessCheck below).
std::wstring restricted(const wchar_t* sddl, bool& changed) {
  Descriptor in; if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &in.value, nullptr)) throw std::runtime_error("descriptor");
  BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr; GetSecurityDescriptorDacl(in.value, &present, &dacl, &defaulted);
  std::vector<BYTE> acl; if (!host::withoutUserDefinedControl(dacl, acl, changed)) throw std::runtime_error("list");
  SECURITY_DESCRIPTOR next{}; InitializeSecurityDescriptor(&next, SECURITY_DESCRIPTOR_REVISION); SetSecurityDescriptorDacl(&next, TRUE, reinterpret_cast<PACL>(acl.data()), FALSE);
  LPWSTR text = nullptr; if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(&next, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr)) throw std::runtime_error("text");
  std::wstring out(text); LocalFree(text); return out;
}
// What a token with only the ordinary groups (an interactive standard user) gets on a service with [dacl].
DWORD grantedToUser(const std::wstring& dacl) {
  Descriptor sd; const std::wstring whole = L"O:SYG:SY" + dacl;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(whole.c_str(), SDDL_REVISION_1, &sd.value, nullptr)) throw std::runtime_error("whole");
  HANDLE process = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process)) throw std::runtime_error("token");
  DWORD size = 0; GetTokenInformation(process, TokenGroups, nullptr, 0, &size); std::vector<BYTE> groups(size);
  if (!GetTokenInformation(process, TokenGroups, groups.data(), size, &size)) throw std::runtime_error("groups");
  const auto* list = reinterpret_cast<TOKEN_GROUPS*>(groups.data()); std::vector<SID_AND_ATTRIBUTES> off;
  for (DWORD i = 0; i < list->GroupCount; ++i) {
    const PSID sid = list->Groups[i].Sid;
    if (IsWellKnownSid(sid, WinWorldSid) || IsWellKnownSid(sid, WinAuthenticatedUserSid) || IsWellKnownSid(sid, WinBuiltinUsersSid) || IsWellKnownSid(sid, WinInteractiveSid)) continue;
    if (list->Groups[i].Attributes & SE_GROUP_INTEGRITY) continue;
    off.push_back({sid, 0});
  }
  HANDLE restrictedToken = nullptr, impersonation = nullptr;
  if (!CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, DWORD(off.size()), off.data(), 0, nullptr, 0, nullptr, &restrictedToken)) throw std::runtime_error("restricted");
  if (!DuplicateTokenEx(restrictedToken, TOKEN_QUERY, nullptr, SecurityIdentification, TokenImpersonation, &impersonation)) throw std::runtime_error("impersonation");
  PRIVILEGE_SET privileges{}; DWORD privilegesSize = sizeof(privileges), mask = 0; BOOL allowed = FALSE;
  const BOOL ok = AccessCheck(sd.value, impersonation, MAXIMUM_ALLOWED, const_cast<GENERIC_MAPPING*>(&serviceMapping), &privileges, &privilegesSize, &mask, &allowed);
  CloseHandle(impersonation); CloseHandle(restrictedToken); CloseHandle(process);
  if (!ok) throw std::runtime_error("AccessCheck");
  return allowed ? mask : 0;
}
}
int main() {
  try {
    // The list Windows gives a new service (Windows 10/11), and one that also names Authenticated Users.
    const wchar_t* defaults = L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)";
    const wchar_t* wider = L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)(A;;CCLCSWLOCRRC;;;AU)(A;;GX;;;WD)";
    check((grantedToUser(defaults) & SERVICE_USER_DEFINED_CONTROL) != 0, "the fixture's user really holds the control on the default list");
    bool changed = false;
    const auto after = restricted(defaults, changed);
    check(changed, "the default list was changed");
    const DWORD user = grantedToUser(after);
    check(!(user & SERVICE_USER_DEFINED_CONTROL), "an interactive user can still send controls 128/129");
    check((user & SERVICE_QUERY_STATUS) && (user & SERVICE_QUERY_CONFIG) && (user & SERVICE_INTERROGATE), "an interactive user lost the status queries it had");
    check(after.find(L"(A;;CCLCSWRPWPDTLOCRRC;;;SY)") != std::wstring::npos && after.find(L"(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)") != std::wstring::npos, "SYSTEM or Administrators lost a right");
    check(after.find(L"(A;;CCLCSWLORC;;;IU)") != std::wstring::npos && after.find(L"(A;;CCLCSWLORC;;;SU)") != std::wstring::npos, "INTERACTIVE / SERVICE kept more or less than the control taken away");
    const auto widerAfter = restricted(wider, changed);
    check(changed && !(grantedToUser(widerAfter) & (SERVICE_USER_DEFINED_CONTROL | SERVICE_START | SERVICE_STOP)), "Authenticated Users or a generic execute right kept a control");
    bool again = true; const auto twice = restricted(after.c_str(), again);
    check(!again && twice == after, "an already restricted list was changed again");
    std::printf("checks=%u failed=%u\n", checks, failed);
    return failed ? 1 : 0;
  } catch (const std::exception& error) { std::printf("error %s\n", error.what()); return 2; }
}
