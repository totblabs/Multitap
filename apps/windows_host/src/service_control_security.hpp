#pragma once
// Who may send the Host service its own controls.
// 128 (reload: drops the broker connection and any stream) and 129 (announce
// capabilities) are SERVICE_USER_DEFINED_CONTROL, which the default service list
// gives to INTERACTIVE and SERVICE logons (and sometimes Authenticated Users):
// any signed-in user could end a stream. Host settings, which sends them, runs
// as an administrator. The service takes that right away from every entry that
// is not SYSTEM or Administrators each time it starts; nothing else in the list
// changes (status queries stay as they are). The MSI's ServiceInstall cannot
// carry a descriptor (no MsiLockPermissionsEx table in this package), so the
// service applies it itself; its own object is owned by SYSTEM, which may
// always write the list.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>
#include <vector>

namespace host {
// A copy of [dacl] in which no allow entry but SYSTEM's and Administrators'
// carries SERVICE_USER_DEFINED_CONTROL (or GENERIC_ALL / GENERIC_EXECUTE, which
// map to it). Returns false when [dacl] cannot be read; [changed] tells whether
// anything was taken away.
inline bool withoutUserDefinedControl(PACL dacl, std::vector<BYTE>& out, bool& changed) {
  changed = false;
  if (!dacl) return false;
  ACL_SIZE_INFORMATION size{};
  if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation)) return false;
  const auto* first = reinterpret_cast<const BYTE*>(dacl);
  out.assign(first, first + size.AclBytesInUse);
  out.resize(size.AclBytesInUse + size.AclBytesFree);
  const auto copy = reinterpret_cast<PACL>(out.data());
  copy->AclSize = WORD(out.size());
  for (DWORD i = 0; i < copy->AceCount; ++i) {
    void* raw = nullptr; if (!GetAce(copy, i, &raw)) return false;
    auto* header = static_cast<ACE_HEADER*>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    const PSID sid = &ace->SidStart;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) continue;
    constexpr ACCESS_MASK control = SERVICE_USER_DEFINED_CONTROL | GENERIC_ALL | GENERIC_EXECUTE;
    if (ace->Mask & control) { ace->Mask &= ~control; changed = true; }
  }
  return true;
}
// Applied by the running service to its own entry. False when Windows refused.
inline bool restrictServiceControls(const wchar_t* name) noexcept {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!manager) return false;
  bool ok = false;
  if (SC_HANDLE service = OpenServiceW(manager, name, READ_CONTROL | WRITE_DAC)) {
    DWORD needed = 0; QueryServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
    if (needed && needed < 65536) {
      std::vector<BYTE> bytes(needed);
      if (QueryServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, bytes.data(), needed, &needed)) {
        BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
        std::vector<BYTE> acl; bool changed = false;
        if (GetSecurityDescriptorDacl(bytes.data(), &present, &dacl, &defaulted) && present && withoutUserDefinedControl(dacl, acl, changed)) {
          if (!changed) ok = true;
          else {
            SECURITY_DESCRIPTOR next{};
            ok = InitializeSecurityDescriptor(&next, SECURITY_DESCRIPTOR_REVISION) &&
                 SetSecurityDescriptorDacl(&next, TRUE, reinterpret_cast<PACL>(acl.data()), FALSE) &&
                 SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, &next);
          }
        }
      }
    }
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  return ok;
}
}  // namespace host
