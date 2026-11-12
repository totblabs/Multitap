#pragma once
// The client installer's texts (MultitapClientSetup: setup_main.cpp, the
// removal window setup_uninstall_ui.hpp, setup_uninstall_reason.hpp) in
// Korean and English, one row per text: X(id, Korean, English). Korean is the
// source; English follows docs/i18n.md. "{0}".."{9}" are placeholders
// (multitap::ui::fill); both languages of a row use the same ones
// (ui_text_test.cpp checks it, and that no English text has Hangul). The
// windows have fixed sizes: an English text is kept as short as its Korean,
// never the window made bigger.
#include "ui_language.hpp"

#define MULTITAP_SETUP_TEXTS(X) \
  /* Confirmation dialogs */ \
  X(NoticeFooter, L"설치 중에는 이 창을 닫을 수 없어요.", L"You cannot close this window while it installs.") \
  X(Ok, L"확인", L"OK") \
  X(Cancel, L"취소", L"Cancel") \
  X(Close, L"닫기", L"Close") \
  X(QaConfirmTitle, L"Multitap 확인창 검사", L"Multitap confirmation check") \
  X(QaConfirmHeading, L"확인창 키보드 검사", L"Confirmation keyboard check") \
  X(QaConfirmBody, L"검사용 화면이에요. 설치와 재시작은 실행하지 않아요.", L"This is a test screen. Nothing is installed or restarted.") \
  X(QaConfirmFooter, L"실제 Windows 설정 변경 없음", L"No real Windows setting changes") \
  X(InstallConfirmTitle, L"설치 내용 확인", L"Confirm installation") \
  X(InstallConfirmHeadingClient, L"Multitap을 설치할까요?", L"Install Multitap?") \
  X(InstallConfirmBodyClient, L"Multitap을 설치하고 바탕 화면에 바로가기를 만들어요.\n서버 주소와 로그인 설정은 그대로예요.", L"Installs Multitap and adds a shortcut to the desktop.\nYour server address and sign-in settings stay as they are.") \
  X(InstallConfirmFooterPreview, L"화면 미리보기예요. 실제로 설치하지 않아요.", L"This is a preview. Nothing is installed.") \
  X(InstallConfirmFooterClient, L"열려 있는 Multitap은 닫히고, 데스크톱 연결도 끊겨요.", L"Open Multitap windows close, and desktop connections end.") \
  X(InstallAction, L"설치", L"Install") \
  X(InstallingTitle, L"설치가 진행 중이에요", L"Installation in progress") \
  X(InstallingNotice, L"끝나면 이 창에서 결과를 알려 드려요.\n잠시 기다려 주세요.", L"This window shows the result when it finishes.\nPlease wait.") \
  /* Diagnostic copy ("진단 복사") */ \
  X(DiagHeader, L"Multitap {0} {1}\r\n완료 단계: {2} / 6\r\n설치 결과: {3}", L"Multitap {0} {1}\r\nSteps completed: {2} / 6\r\nResult: {3}") \
  X(DiagFailed, L"실패", L"Failed") \
  X(DiagDone, L"완료", L"Done") \
  X(DiagRestart, L"\r\nWindows 다시 시작 필요: {0}\r\n다시 시작 요청 수락: {1}", L"\r\nWindows restart needed: {0}\r\nRestart request accepted: {1}") \
  X(DiagHasNot, L"없음", L"no") \
  X(DiagNo, L"아니요", L"no") \
  /* Steps (the Working view) */ \
  X(ClientStep1, L"설치 파일·대상 경로 확인", L"Check setup files and destination") \
  X(ClientStep2, L"프로그램 파일 배치", L"Place program files") \
  X(ClientStep3, L"설치 정보 확인", L"Check installation details") \
  X(ClientStep4, L"바로가기 생성", L"Create the shortcut") \
  X(ClientStep5, L"설치된 파일 확인", L"Check installed files") \
  X(ClientStep6, L"클라이언트 준비", L"Get the client ready") \
  /* Steps (the update splash) */ \
  X(ClientSplash1, L"설치 파일과 대상 경로를 확인하고 있어요", L"Checking the setup files and destination") \
  X(ClientSplash2, L"프로그램 파일을 배치하고 있어요", L"Placing the program files") \
  X(ClientSplash3, L"설치 정보를 확인하고 있어요", L"Checking the installation details") \
  X(ClientSplash4, L"바로가기를 만들고 있어요", L"Creating the shortcut") \
  X(ClientSplash5, L"설치된 파일을 검증하고 있어요", L"Verifying the installed files") \
  X(ClientSplash6, L"새 버전을 준비하고 있어요", L"Preparing the new version") \
  X(SplashWindowTitle, L"Multitap 업데이트 적용 중", L"Applying the Multitap update") \
  X(SplashFinishing, L"업데이트를 마무리하고 있어요", L"Finishing the update") \
  X(SplashApplying, L"업데이트 적용 중…", L"Applying the update…") \
  X(SplashChecking, L"설치 프로그램을 확인하고 있어요", L"Checking the installer") \
  X(SplashDone, L"완료 · 새 버전을 열고 있어요", L"Done · Opening the new version") \
  X(SplashNoteClient, L"끝나면 Multitap이 같은 자리에서 다시 열려요.", L"Multitap opens again in the same place when it finishes.") \
  /* Installer stopped without a result */ \
  X(Interrupted, L"설치 프로그램이 결과를 남기지 않고 끝나서 설치를 마치지 못했어요.\r\n멈춘 단계: {0}/{1} · {2}\r\n보안 프로그램이 설치 과정을 중단했을 수 있어요. 보안 프로그램의 차단 기록을 확인해 보세요.", L"The installer ended without a result, so the installation did not finish.\r\nStopped at step {0}/{1} · {2}\r\nA security program may have stopped the installation. Check its blocking history.") \
  X(InterruptedClientRetry, L"\r\n‘다시 시도’로 안 되면 설치 파일을 다시 실행하세요.", L"\r\nIf ‘Try again’ does not help, run the setup file again.") \
  X(DowngradeClient, L"설치된 Multitap보다 낮은 버전이라 자동 업데이트하지 않았어요.\r\n이전 버전이 필요하면 그 버전의 설치 파일을 직접 실행하세요. 설치된 앱은 그대로예요.", L"This version is older than the installed Multitap, so it was not updated automatically.\r\nTo go back to an earlier version, run that version's setup file yourself. The installed app stays as it is.") \
  /* Window titles */ \
  X(TitlePreview, L"Multitap 설치 화면 미리보기", L"Multitap setup preview") \
  X(TitleSelfTest, L"Multitap 설치 화면 검사", L"Multitap setup check") \
  X(TitleClient, L"Multitap 설치", L"Multitap setup") \
  X(TitleClientUpdate, L"Multitap 업데이트", L"Multitap update") \
  /* Design previews (--preview-*) */ \
  X(PreviewProgress, L"서버 등록 정보 보존 완료 · 가상 화면을 준비하고 있어요.", L"Server registration kept · Preparing the virtual display.") \
  X(PreviewFailure, L"설치 대상 파일을 다른 프로그램이 쓰고 있어요.\r\n열려 있는 Multitap 설정 창을 닫은 뒤 다시 시도하세요.\r\n기존 등록 정보는 그대로예요.", L"Another program is using a file this setup has to replace.\r\nClose open Multitap settings windows, then try again.\r\nThe existing registration stays as it is.") \
  /* Results */ \
  X(RelaunchFailedClient, L"Multitap {0} 업데이트했어요.\r\n새 버전을 자동으로 열지 못했어요. 바탕 화면에서 Multitap을 여세요.", L"Updated Multitap to {0}.\r\nThe new version did not open by itself. Open Multitap from the desktop.") \
  X(NoteRelaunchFailed, L"새 버전을 자동으로 열지 못했어요", L"The new version did not open by itself") \
  X(NoteInterrupted, L"설치 프로그램이 결과 없이 끝났어요", L"The installer ended without a result") \
  X(NoteInterruptedDetail, L"{0}/{1}단계 ‘{2}’에서 멈췄어요. 보안 프로그램이 중단했을 수 있으니 차단 기록을 확인해 보세요.", L"It stopped at step {0}/{1}, ‘{2}’. A security program may have stopped it; check its blocking history.") \
  X(NoteCanReinstall, L"다시 설치할 수 있어요", L"You can install again") \
  X(NoteCanReinstallDetail, L"‘다시 시도’로 안 되면 설치 파일을 다시 실행하세요.", L"If ‘Try again’ does not help, run the setup file again.") \
  X(HeadingUpdateFailed, L"업데이트하지 못했어요", L"Could not update") \
  X(HeadingInstallFailed, L"설치하지 못했어요", L"Could not install") \
  X(HeadingFailedNext, L"아래 내용을 확인한 뒤 다시 시도하세요.", L"Check the details below, then try again.") \
  X(HeadingUpdateDone, L"업데이트를 마쳤어요", L"Update complete") \
  X(HeadingInstallDone, L"설치를 마쳤어요", L"Installation complete") \
  X(NextOpenClient, L"바탕 화면에서 Multitap을 직접 여세요.", L"Open Multitap from the desktop yourself.") \
  X(NextSignIn, L"Multitap에서 로그인하세요.", L"Sign in from Multitap.") \
  X(DetailClientDone, L"바탕 화면의 Multitap을 실행하세요. 기존 서버 주소와 로그인 기억 설정은 그대로예요.", L"Start Multitap from the desktop. Your server address and remembered sign-in stay as they are.") \
  X(Copied, L"복사됨", L"Copied") \
  X(CopyAgain, L"다시 복사", L"Copy again") \
  X(CopyDiagnostic, L"진단 복사", L"Copy report") \
  X(StartingUpdate, L"업데이트를 시작하고 있어요…", L"Starting the update…") \
  X(StartingInstall, L"설치를 시작하고 있어요…", L"Starting the installation…") \
  X(PreparingInstall, L"설치를 준비하고 있어요", L"Getting ready to install") \
  /* The main window */ \
  X(HeaderUpdate, L" · 업데이트", L" · Update") \
  X(HeaderInstall, L" · 설치", L" · Install") \
  X(PreviewBadge, L"화면 미리보기", L"Preview") \
  X(WelcomeTitleClient, L"Multitap 클라이언트 설치", L"Install Multitap Client") \
  X(WelcomeSubtitleClient, L"Multitap을 설치하고 바탕 화면에 바로가기를 만들어요.", L"Installs Multitap and adds a shortcut to the desktop.") \
  X(CardClient1, L"클라이언트 파일", L"Client files") \
  X(CardClient2, L"서버·로그인 설정", L"Server and sign-in settings") \
  X(CardClient3, L"바탕 화면 바로가기", L"Desktop shortcut") \
  X(CardClientDetail1, L"필요한 구성 요소를 함께 설치", L"Installed with the components it needs") \
  X(CardClientDetail2, L"기존 설정을 그대로 유지", L"Your existing settings stay") \
  X(CardClientDetail3, L"바탕 화면 바로가기 추가", L"Adds a desktop shortcut") \
  X(WelcomeNoteClient, L"설치할 때 열려 있는 Multitap은 닫혀요.", L"Open Multitap windows close when it installs.") \
  X(WelcomeFooterClient, L"Windows를 다시 시작하지 않아요", L"Windows does not restart") \
  X(WorkingTitleUpdate, L"업데이트하고 있어요", L"Updating") \
  X(WorkingTitleInstall, L"설치하고 있어요", L"Installing") \
  X(WorkingSubtitleClientUpdate, L"끝나면 새 버전이 자동으로 열려요.", L"The new version opens when it finishes.") \
  X(WorkingSubtitleInstall, L"끝날 때까지 이 창을 열어 두세요.", L"Keep this window open until it finishes.") \
  X(InstallLocation, L"설치 위치: {0}", L"Install location: {0}") \
  X(ButtonClosePreview, L"미리보기 닫기", L"Close preview") \
  X(ButtonUpdating, L"업데이트 중…", L"Updating…") \
  X(ButtonInstalling, L"설치 중…", L"Installing…") \
  X(ButtonStartInstall, L"설치 시작", L"Start install") \
  X(ButtonTryAgain, L"다시 시도", L"Try again") \
  X(ButtonCollapse, L"접기  −", L"Hide  −") \
  X(ButtonDetails, L"자세히 보기  +", L"Show details  +") \
  X(SetupFileMissing, L"설치에 필요한 파일이 없어요. 설치 파일을 다시 받아 실행하세요.", L"A file the setup needs is missing. Download the setup file again and run it.") \
  X(PrepareFailed, L"설치를 준비하지 못했어요.", L"Could not prepare the installation.") \
  X(StartFailed, L"설치를 시작하지 못했어요.", L"Could not start the installation.") \
  X(ClientOpenFailed, L"\r\n클라이언트를 자동으로 열지 못했어요. 바탕 화면의 Multitap을 실행하세요.", L"\r\nMultitap did not open by itself. Start Multitap from the desktop.") \
  /* Shortcut (its tooltip) */ \
  X(ClientShortcutDescription, L"Multitap 데스크톱 연결", L"Multitap desktop connection") \
  /* Removal window */ \
  X(RemoveTitleClient, L"Multitap 제거", L"Remove Multitap") \
  X(RemoveFailedHeading, L"제거를 마치지 못했어요", L"Could not finish removing") \
  X(RemovedClient, L"Multitap을 제거했어요", L"Multitap is removed") \
  X(RemovingClient, L"Multitap을 제거하고 있어요", L"Removing Multitap") \
  X(RemoveAskClient, L"Multitap을 제거할까요?", L"Remove Multitap?") \
  X(RemovedBody, L"서버 주소와 로그인 설정은 그대로예요.", L"Your server address and sign-in settings stay.") \
  X(RemovingBody, L"Multitap을 닫고 설치한 파일만 제거해요.\n잠시 기다려 주세요.", L"Multitap closes and only its installed files are removed.\nPlease wait.") \
  X(RemoveAskBodyClient, L"연결 중인 데스크톱이 있으면 연결이 끊겨요.\n서버 주소와 로그인 설정은 남아요.", L"Any desktop you are connected to is disconnected.\nYour server address and sign-in settings stay.") \
  X(RemoveCode, L"확인 코드: {0}", L"Code: {0}") \
  X(RemoveUnchanged, L"개인 파일과 Windows 설정은 변경하지 않아요.", L"Your personal files and Windows settings are not changed.") \
  X(RemoveAction, L"제거", L"Remove") \
  X(RemovalPartial, L"제거 중 일부 파일이 남았어요.\n프로그램을 다시 설치한 뒤 제거하세요.", L"Some files were left behind.\nInstall the program again, then remove it.") \
  X(RemovalInUse, L"프로그램이 사용 중이라 제거를 멈췄어요.\n열려 있는 Multitap 창을 닫고 다시 시도하세요.", L"Removal stopped because the program is in use.\nClose open Multitap windows and try again.") \
  X(RemovalOtherInstaller, L"다른 Multitap 설치나 제거가 진행 중이에요.\n끝난 뒤 다시 제거하세요.", L"Another Multitap install or removal is running.\nRemove it again when that is done.") \
  X(RemovalInstallation, L"설치 정보를 확인하지 못했어요.\n프로그램을 다시 설치한 뒤 제거하세요.", L"Could not read the installation details.\nInstall the program again, then remove it.")

namespace multitap::setup_text {
enum class Id : unsigned {
#define MULTITAP_SETUP_TEXT_ID(id, ko, en) id,
  MULTITAP_SETUP_TEXTS(MULTITAP_SETUP_TEXT_ID)
#undef MULTITAP_SETUP_TEXT_ID
  Count
};
struct Entry { const char* name; const wchar_t* korean; const wchar_t* english; };
inline constexpr Entry entries[] = {
#define MULTITAP_SETUP_TEXT_ENTRY(id, ko, en) {#id, ko, en},
  MULTITAP_SETUP_TEXTS(MULTITAP_SETUP_TEXT_ENTRY)
#undef MULTITAP_SETUP_TEXT_ENTRY
};
static_assert(sizeof(entries) / sizeof(entries[0]) == size_t(Id::Count), "every setup text id has one row");
inline const wchar_t* text(Id id, ui::Language language) {
  const auto& entry = entries[size_t(id)];
  return language == ui::Language::korean ? entry.korean : entry.english;
}
// The text in this process's language.
inline const wchar_t* text(Id id) { return text(id, ui::processLanguage()); }
inline std::wstring format(Id id, std::initializer_list<std::wstring> arguments) { return ui::fill(text(id), arguments); }
}  // namespace multitap::setup_text
