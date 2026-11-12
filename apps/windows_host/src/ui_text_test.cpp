// Korean and English text tables of the native Host parts (docs/i18n.md):
// setup_text.hpp (the client installer's windows), host_text.hpp (tray, service,
// settings helper) and installer/host_installer_text.hpp (the Host installer's
// message boxes). Memory only: nothing is shown, installed or changed.
//  * every id has a Korean and an English text; English has no Hangul and no
//    exclamation mark, Korean has Hangul unless both are the same words;
//  * both languages of a row take the same "{n}" placeholders, and fill()
//    puts data in once without reading it as a pattern;
//  * the process language follows the forced (test) language both ways.
#include "setup_text.hpp"
#include "host_text.hpp"
#include "../installer/host_installer_text.hpp"
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>

namespace {
unsigned checks = 0, failed = 0;
void check(bool value, const std::string& name) { ++checks; if (!value) { ++failed; std::printf("FAIL %s\n", name.c_str()); } }

template <class Entry, size_t N> void table(const char* kind, const Entry (&entries)[N]) {
  std::set<std::string> names;
  for (const auto& entry : entries) {
    const std::string name = std::string(kind) + "." + entry.name;
    const std::wstring ko = entry.korean ? entry.korean : L"", en = entry.english ? entry.english : L"";
    check(names.insert(entry.name).second, name + " is unique");
    check(!ko.empty(), name + " has Korean");
    check(!en.empty(), name + " has English");
    check(!multitap::ui::hasHangul(en), name + " English has no Hangul");
    check(en.find(L'!') == std::wstring::npos, name + " English has no exclamation mark");
    // Korean has Hangul, unless it is only product names and placeholders ("Host {0}").
    const auto onlyNames = [](std::wstring v) {
      for (const wchar_t* word : {L"Multitap", L"Host"}) for (size_t at; (at = v.find(word)) != std::wstring::npos;) v.erase(at, std::wcslen(word));
      for (const wchar_t ch : v) if (!(ch == L' ' || ch == L'{' || ch == L'}' || (ch >= L'0' && ch <= L'9') || ch == L'·')) return false;
      return true;
    };
    check(multitap::ui::hasHangul(ko) || onlyNames(ko), name + " Korean is Korean");
    check(multitap::ui::placeholders(ko.c_str()) == multitap::ui::placeholders(en.c_str()), name + " same placeholders");
    // Same line structure where the Korean has a fixed one (dialogs size their lines).
    const auto lines = [](const std::wstring& v) { size_t n = 0; for (const wchar_t ch : v) if (ch == L'\n') ++n; return n; };
    check(lines(ko) == lines(en), name + " same line breaks");
    // Product wording (docs/i18n.md): "PC", never "this PC" / "my PC".
    check(en.find(L"this PC") == std::wstring::npos && en.find(L"my PC") == std::wstring::npos, name + " English says PC");
    check(ko.find(L"이 PC") == std::wstring::npos && ko.find(L"내 PC") == std::wstring::npos, name + " Korean says PC");
  }
  std::printf("%s rows=%zu\n", kind, N);
}
}  // namespace

int main() {
  using multitap::ui::Language;
  table("setup", multitap::setup_text::entries);
  table("host", host::ui_text::entries);
  table("installer", multitap::installer_text::kTexts);

  // Placeholders: once, in order of the pattern, data never re-read.
  check(multitap::ui::fill(L"{0}/{1} · {2}", {L"4", L"6", L"x"}) == L"4/6 · x", "fill in order");
  check(multitap::ui::fill(L"a{0}b", {L"{1}{0}%s"}) == L"a{1}{0}%sb", "data is not a pattern");
  check(multitap::ui::fill(L"{0} {1} {x} {", {L"one"}) == L"one {1} {x} {", "unknown placeholders stay");
  check(multitap::ui::placeholders(L"{0} and {2}") == 5u && multitap::ui::placeholders(L"none") == 0u, "placeholder sets");

  // Which language: Korean only for ko / ko-*, else English.
  check(multitap::ui::languageFromTag(L"ko") == Language::korean && multitap::ui::languageFromTag(L"ko-KR") == Language::korean && multitap::ui::languageFromTag(L"KO_kr") == Language::korean, "ko tags");
  check(multitap::ui::languageFromTag(L"en-US") == Language::english && multitap::ui::languageFromTag(L"kok") == Language::english && multitap::ui::languageFromTag(L"") == Language::english && multitap::ui::languageFromTag(nullptr) == Language::english, "other tags");
  check(multitap::ui::languageFromId(MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN)) == Language::korean && multitap::ui::languageFromId(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) == Language::english && multitap::ui::languageFromId(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)) == Language::english, "LANGIDs");

  // Forced English: every lookup of every part follows, and the Korean source is untouched.
  multitap::ui::forceLanguageForTest(Language::english);
  check(!multitap::ui::korean() && multitap::ui::sessionLanguage(0xFFFFFFFF) == Language::english && multitap::ui::sidLanguage(L"S-1-5-18") == Language::english, "forced English");
  check(std::wstring(multitap::setup_text::text(multitap::setup_text::Id::ButtonStartInstall)) == L"Start install", "setup English");
  check(std::wstring(host::ui_text::get(host::ui_text::Id::TrayOpenSettings)) == L"Open Host settings", "tray English");
  check(host::ui_text::utf8(host::ui_text::Id::RpcUnsupported) == "This settings request is not supported.", "settings helper English");
  check(multitap::setup_text::format(multitap::setup_text::Id::DiagHeader, {L"Client", L"1.0.0", L"4", multitap::setup_text::text(multitap::setup_text::Id::DiagDone)}) == L"Multitap Client 1.0.0\r\nSteps completed: 4 / 6\r\nResult: Done", "diagnostic English");
  check(multitap::ui::fill(multitap::installer_text::text(multitap::installer_text::Text::Done, Language::english), {L"1.0.0"}) == L"Multitap Host 1.0.0 is installed.", "installer English");
  for (size_t i = 0; i < size_t(multitap::setup_text::Id::Count); ++i) check(!multitap::ui::hasHangul(multitap::setup_text::text(multitap::setup_text::Id(i))), std::string("setup English lookup ") + multitap::setup_text::entries[i].name);
  for (size_t i = 0; i < size_t(host::ui_text::Id::Count); ++i) check(!multitap::ui::hasHangul(host::ui_text::get(host::ui_text::Id(i))), std::string("host English lookup ") + host::ui_text::entries[i].name);

  // Forced Korean: the source text exactly as these windows showed it before.
  multitap::ui::forceLanguageForTest(Language::korean);
  check(multitap::ui::korean() && multitap::ui::sessionLanguage(0xFFFFFFFF) == Language::korean, "forced Korean");
  check(multitap::setup_text::format(multitap::setup_text::Id::DiagHeader, {L"Client", L"1.0.0", L"4", multitap::setup_text::text(multitap::setup_text::Id::DiagDone)}) == L"Multitap Client 1.0.0\r\n완료 단계: 4 / 6\r\n설치 결과: 완료", "diagnostic Korean");
  check(multitap::setup_text::format(multitap::setup_text::Id::Interrupted, {L"4", L"6", multitap::setup_text::text(multitap::setup_text::Id::ClientStep4)}).find(L"멈춘 단계: 4/6 · 바로가기 생성") != std::wstring::npos, "interrupted Korean");
  check(host::ui_text::format(host::ui_text::Id::TrayMenuStatus, {host::ui_text::get(host::ui_text::Id::ServiceRunning)}) == L"Host 서비스 실행 중", "tray Korean");
  check(multitap::ui::fill(multitap::installer_text::text(multitap::installer_text::Text::Done, Language::korean), {L"1.0.0"}) == L"Multitap Host 1.0.0 설치를 마쳤어요.", "installer Korean");
  for (size_t i = 0; i < size_t(host::ui_text::Id::Count); ++i) check(std::wstring(host::ui_text::get(host::ui_text::Id(i))) == host::ui_text::entries[i].korean, std::string("host Korean lookup ") + host::ui_text::entries[i].name);
  multitap::ui::forceLanguageForTest(std::nullopt);

  std::printf("{\"kind\":\"native-ui-text\",\"checks\":%u,\"failed\":%u}\n", checks, failed);
  return failed ? 1 : 0;
}
