#pragma once
#include <windows.h>

namespace multitap {
inline bool administratorToken() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID sid = nullptr;
  if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
      DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) return false;
  BOOL member = FALSE;
  const BOOL queried = CheckTokenMembership(nullptr, sid, &member);
  FreeSid(sid);
  return queried && member;
}
inline bool requireAdministratorWindow() {
  if (administratorToken()) return true;
  MessageBoxW(nullptr, L"이 프로그램은 관리자 권한으로 실행해야 합니다.\n실행 시 Windows 관리자 승인을 완료해주세요.",
      L"Multitap · 관리자 권한 필요", MB_OK | MB_ICONERROR);
  return false;
}
}
