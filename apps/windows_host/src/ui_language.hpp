#pragma once
// Korean and English (docs/i18n.md). The Host, its
// tray, the installers and every text a native Host part shows a person pick
// one language: Korean when the Windows display language is Korean, English
// otherwise. Korean stays the source text; each part keeps its texts in one
// table with both languages side by side (setup_text.hpp, host_text.hpp, the
// installer's own table) and formats placeholders with fill() below.
//
// Which Windows language:
//  * an ordinary process (installers, tray, Host settings' helper): its own
//    user's display language, GetUserDefaultUILanguage().
//  * a process that runs as SYSTEM but shows a text to the signed-in person
//    (the service's power notice, the session printer's comment): that
//    session user's choice, sessionLanguage() below, not SYSTEM's.
// No product setting overrides it (docs/i18n.md: "none (follows Windows)").
// Tests force one language with forceLanguageForTest(); QA builds of the
// installer also read MULTITAP_QA_LANG (setup_main.cpp, QA targets only).
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>
#pragma comment(lib,"wtsapi32.lib")
#pragma comment(lib,"advapi32.lib")

namespace multitap::ui {
enum class Language { korean, english };

inline Language languageFromId(LANGID id) { return PRIMARYLANGID(id) == LANG_KOREAN ? Language::korean : Language::english; }
// "ko", "ko-KR", "KO_kr" -> Korean; anything else (or nothing) -> English.
inline Language languageFromTag(const wchar_t* tag) {
  if (!tag) return Language::english;
  const wchar_t a = tag[0], b = a ? tag[1] : 0, c = b ? tag[2] : 0;
  return (a == L'k' || a == L'K') && (b == L'o' || b == L'O') && (c == 0 || c == L'-' || c == L'_') ? Language::korean : Language::english;
}
inline std::optional<Language>& forcedLanguage() { static std::optional<Language> value; return value; }
// Tests only: every later text of this process is in [value] (nullopt: Windows again).
inline void forceLanguageForTest(std::optional<Language> value) { forcedLanguage() = value; }
// This process's language: its user's Windows display language.
inline Language processLanguage() {
  if (forcedLanguage()) return *forcedLanguage();
  return languageFromId(GetUserDefaultUILanguage());
}
inline bool korean() { return processLanguage() == Language::korean; }

// The display language of the person signed in to [session], for a SYSTEM
// process (the service) that shows them a text. A user who chose a display
// language has it first in HKEY_USERS\<SID>\Control Panel\Desktop\
// PreferredUILanguages; without that choice Windows shows its own install
// language, GetSystemDefaultUILanguage(). Read only.
inline Language sidLanguage(const std::wstring& sid) {
  if (forcedLanguage()) return *forcedLanguage();
  if (!sid.empty() && sid.size() < 200 && sid.find(L'\\') == std::wstring::npos) {
    const std::wstring key = sid + L"\\Control Panel\\Desktop";
    wchar_t value[512]{}; DWORD bytes = sizeof(value) - 2 * sizeof(wchar_t);
    if (RegGetValueW(HKEY_USERS, key.c_str(), L"PreferredUILanguages", RRF_RT_REG_MULTI_SZ, nullptr, value, &bytes) == ERROR_SUCCESS && value[0])
      return languageFromTag(value);
  }
  return languageFromId(GetSystemDefaultUILanguage());
}
inline Language userTokenLanguage(HANDLE token) {
  if (forcedLanguage()) return *forcedLanguage();
  std::wstring text;
  if (token) {
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size && size < 4096) {
      std::vector<BYTE> buffer(size);
      LPWSTR sid = nullptr;
      if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) { text = sid; LocalFree(sid); }
    }
  }
  return sidLanguage(text);
}
inline Language sessionLanguage(DWORD session) {
  if (forcedLanguage()) return *forcedLanguage();
  HANDLE token = nullptr;
  if (session != 0xFFFFFFFF && WTSQueryUserToken(session, &token)) { const auto value = userTokenLanguage(token); CloseHandle(token); return value; }
  return languageFromId(GetSystemDefaultUILanguage());
}
// The person at the physical console (or the one active session), for a
// notice the whole machine shows (a restart or shutdown message).
inline Language consoleLanguage() { return sessionLanguage(WTSGetActiveConsoleSessionId()); }

// One string per message (docs/i18n.md): "{0}".."{9}" in [pattern] are
// replaced by the arguments, once. Arguments are data: they are never
// scanned again, so a brace in a path or a JSON snapshot stays as it is.
// Anything else in the pattern, an unknown "{x}" included, is kept.
inline std::wstring fill(const wchar_t* pattern, std::initializer_list<std::wstring> arguments) {
  std::wstring out; if (!pattern) return out;
  const auto* args = arguments.begin(); const size_t count = arguments.size();
  for (const wchar_t* p = pattern; *p; ++p) {
    if (p[0] == L'{' && p[1] >= L'0' && p[1] <= L'9' && p[2] == L'}' && size_t(p[1] - L'0') < count) { out += args[p[1] - L'0']; p += 2; continue; }
    out += *p;
  }
  return out;
}
inline bool hasHangul(const std::wstring& value) {
  for (const wchar_t ch : value) if ((ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0x1100 && ch <= 0x11FF) || (ch >= 0x3130 && ch <= 0x318F)) return true;
  return false;
}
// The "{n}" placeholders a pattern uses, as a bit set (table checks: both
// languages of one text take the same arguments).
inline unsigned placeholders(const wchar_t* pattern) {
  unsigned bits = 0;
  for (const wchar_t* p = pattern; p && *p; ++p) if (p[0] == L'{' && p[1] >= L'0' && p[1] <= L'9' && p[2] == L'}') bits |= 1u << (p[1] - L'0');
  return bits;
}
}  // namespace multitap::ui
