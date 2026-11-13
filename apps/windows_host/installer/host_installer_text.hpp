#pragma once
// The Host installer's texts (host_installer.cpp, Multitap-Host-<version>-windows-x64.exe):
// its message boxes and its status window, in Korean and English (docs/i18n.md). X(id, Korean, English); Korean
// is the source and stays exactly the text shown before; "{0}".."{9}" are placeholders
// (multitap::ui::fill). apps/windows_host/src/ui_text_test.cpp checks every row.
#include "../src/ui_language.hpp"
#define MULTITAP_INSTALLER_TEXTS(X) \
  X(Title, L"Multitap Host 설치", L"Multitap Host setup") \
  X(Cancelled, L"설치를 취소했어요.", L"The installation was cancelled.") \
  X(OtherInstall, L"다른 프로그램이 설치 중이에요. 그 설치가 끝난 뒤 다시 실행해 주세요.", L"Another program is installing. Run this again when that installation is done.") \
  X(SameVersion, L"이 버전의 Multitap Host가 이미 설치돼 있어요.", L"This version of Multitap Host is already installed.") \
  X(Unsupported, L"이 Windows에서는 설치할 수 없어요. 64비트 Windows가 필요해요.", L"It cannot be installed on this Windows. 64-bit Windows is required.") \
  X(RolledBack, L"설치를 마치지 못해 설치 전 상태로 되돌렸어요.", L"The installation did not finish, so everything was put back as it was.") \
  X(TimedOut, L"설치가 30분 안에 끝나지 않았어요.", L"The installation did not finish within 30 minutes.") \
  X(Failed, L"설치하지 못했어요.", L"Could not install.") \
  X(UnknownOption, L"알 수 없는 실행 옵션이에요. 옵션 없이 실행해 주세요.", L"Unknown option. Run it without options.") \
  X(AlreadyRunning, L"Multitap Host 설치가 이미 실행 중이에요.", L"Multitap Host setup is already running.") \
  X(NeedsAdmin, L"관리자 권한이 필요해요. 설치 파일을 다시 실행하고 승인해 주세요.", L"Administrator rights are needed. Run the setup file again and approve it.") \
  X(Unpack, L"설치 파일을 풀지 못했어요. 디스크 공간을 확인한 뒤 다시 실행해 주세요.", L"Could not unpack the setup file. Check the free disk space, then run it again.") \
  X(Done, L"Multitap Host {0} 설치를 마쳤어요.", L"Multitap Host {0} is installed.") \
  X(Location, L"\n설치 위치: {0}", L"\nInstall location: {0}") \
  X(Restart, L"\n\n설정을 마치려면 Windows를 다시 시작해 주세요.", L"\n\nRestart Windows to finish the setup.") \
  X(OpenSettings, L"\n\n시작 메뉴나 바탕 화면의 Multitap Host를 열어 설치를 마무리해 주세요.", L"\n\nOpen Multitap Host from the Start menu or the desktop to finish the installation.") \
  X(ErrorDetail, L"\n\n오류 번호: {0}\n기록 파일: {1}", L"\n\nError number: {0}\nLog file: {1}") \
  /* The status window: what the installer is doing while Windows Installer's own progress window is not on screen. */ \
  X(StatusUnpacking, L"설치 파일을 준비하고 있어요", L"Getting the setup files ready") \
  X(StatusClosing, L"실행 중인 Multitap Host를 닫고 있어요", L"Closing Multitap Host programs that are running") \
  X(StatusStarting, L"Windows Installer를 시작하고 있어요", L"Starting Windows Installer") \
  X(StatusInstalling, L"Multitap Host를 설치하고 있어요", L"Installing Multitap Host") \
  X(StatusFinishing, L"설치를 마무리하고 있어요", L"Finishing the installation") \
  X(StatusOpening, L"Host 설정을 열고 있어요", L"Opening Host settings") \
  X(StatusWait, L"창이 바뀔 때까지 잠시만 기다려 주세요.", L"Please wait until the next window opens.") \
  X(StatusVersion, L"Multitap Host {0}", L"Multitap Host {0}")
namespace multitap::installer_text {
enum class Text : unsigned {
#define MULTITAP_INSTALLER_TEXT_ID(id, ko, en) id,
  MULTITAP_INSTALLER_TEXTS(MULTITAP_INSTALLER_TEXT_ID)
#undef MULTITAP_INSTALLER_TEXT_ID
  Count
};
struct TextEntry { const char* name; const wchar_t* korean; const wchar_t* english; };
inline constexpr TextEntry kTexts[] = {
#define MULTITAP_INSTALLER_TEXT_ENTRY(id, ko, en) {#id, ko, en},
  MULTITAP_INSTALLER_TEXTS(MULTITAP_INSTALLER_TEXT_ENTRY)
#undef MULTITAP_INSTALLER_TEXT_ENTRY
};
static_assert(sizeof(kTexts) / sizeof(kTexts[0]) == size_t(Text::Count), "every installer text has one row");
inline const wchar_t* text(Text id, ui::Language language) { const auto& entry = kTexts[size_t(id)]; return language == ui::Language::korean ? entry.korean : entry.english; }
}  // namespace multitap::installer_text
