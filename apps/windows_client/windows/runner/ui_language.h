#pragma once

// The language of the runner's own text (tray menu, message boxes, the
// transfer notice's buttons, Console and file dialogs, USB device names):
// the app's "언어 / Language" choice, which the Dart side stores as
// "language" in %LOCALAPPDATA%\Multitap\appearance.json, else the Windows
// UI language (docs/i18n.md: Korean when it is Korean, English otherwise).
// MULTITAP_UI_LANGUAGE=ko|en overrides both (native tests).

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace multitap {

inline bool ParseUiLanguage(const std::string& text, bool* korean) {
  const auto key = text.find("\"language\"");
  if (key == std::string::npos) return false;
  const auto colon = text.find(':', key);
  if (colon == std::string::npos) return false;
  const auto open = text.find('"', colon);
  if (open == std::string::npos) return false;
  const auto close = text.find('"', open + 1);
  if (close == std::string::npos) return false;
  const std::string value = text.substr(open + 1, close - open - 1);
  if (value == "ko") { *korean = true; return true; }
  if (value == "en") { *korean = false; return true; }
  return false;  // "auto" or anything else: the system's
}

inline bool ReadUiKorean() {
  char* forced = nullptr;
  size_t forcedLength = 0;
  if (_dupenv_s(&forced, &forcedLength, "MULTITAP_UI_LANGUAGE") == 0 && forced) {
    const std::string value(forced);
    free(forced);
    if (value == "ko") return true;
    if (value == "en") return false;
  }
  wchar_t* local = nullptr;
  size_t localLength = 0;
  if (_wdupenv_s(&local, &localLength, L"LOCALAPPDATA") == 0 && local) {
    const std::wstring path = std::wstring(local) + L"\\Multitap\\appearance.json";
    free(local);
    std::ifstream in(path, std::ios::binary);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      bool korean = false;
      if (text.size() < 65536 && ParseUiLanguage(text, &korean)) return korean;
    }
  }
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
}

// Re-read at most every two seconds, so a change in the app's settings shows
// up in the next menu or notice without a restart.
inline bool UiKorean() {
  static std::atomic<ULONGLONG> checked{0};
  static std::atomic<bool> korean{true};
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG last = checked.load();
  if (last == 0 || now - last >= 2000) {
    korean = ReadUiKorean();
    checked = now == 0 ? 1 : now;
  }
  return korean.load();
}

inline const wchar_t* Tr(const wchar_t* korean, const wchar_t* english) {
  return UiKorean() ? korean : english;
}

inline const char* TrA(const char* korean, const char* english) {
  return UiKorean() ? korean : english;
}

}  // namespace multitap
