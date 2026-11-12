#pragma once
// The texts MultitapHost.exe / MultitapHostTool.exe show a person, in Korean
// and English (docs/i18n.md): the tray, the service status words it shows,
// the stream's secure-desktop notice picture, the Windows shutdown notice,
// the session printer's comment, and the errors Host settings shows from its
// settings helper. One row per text: X(id, Korean, English). Korean is the
// source and stays exactly the text shown before. Codes and log words are not
// here: they stay as they are.
//
// Which language (ui_language.hpp): the tray, the worker (the notice) and the
// settings helper run as the signed-in person and use their Windows display
// language. The service runs as SYSTEM: what it shows a person (the shutdown
// notice, the printer it creates for a session) takes that session user's
// language (ui::consoleLanguage(), ui::sessionLanguage()). The settings
// helper follows Host settings when it passes MULTITAP_LANG=ko|en.
#include "ui_language.hpp"

#define MULTITAP_HOST_TEXTS(X) \
  /* Tray */ \
  X(TrayTip, L"Multitap Host · {0}", L"Multitap Host · {0}") \
  X(TrayMenuStatus, L"Host {0}", L"Host · {0}") \
  X(TrayOpenSettings, L"Host 설정 열기", L"Open Host settings") \
  X(TrayRefresh, L"상태 새로고침", L"Refresh status") \
  X(TrayClose, L"트레이 아이콘 닫기", L"Close tray icon") \
  X(TrayCloseBody, L"트레이 아이콘만 닫아요.\nHost 서비스와 연결은 그대로 유지돼요.\n다음에 Windows에 로그인하면 아이콘이 다시 나타나요.", L"Only the tray icon closes.\nThe Host service and connections keep running.\nThe icon comes back the next time you sign in to Windows.") \
  X(TraySettingsMissing, L"Host 설정을 찾지 못했어요. Host 설치 파일을 다시 실행하세요.", L"Host settings was not found. Run the Host setup file again.") \
  X(TraySettingsFailed, L"Host 설정을 열지 못했어요. 다시 시도하세요.", L"Could not open Host settings. Try again.") \
  /* The Host service's state (tray) */ \
  X(ServiceUnknown, L"서비스 상태 확인 필요", L"Service status unknown") \
  X(ServiceNotInstalled, L"서비스 미설치", L"Service not installed") \
  X(ServiceRunning, L"서비스 실행 중", L"Service running") \
  X(ServiceStopped, L"서비스 중지됨", L"Service stopped") \
  X(ServiceStarting, L"서비스 시작 중", L"Service starting") \
  X(ServiceStopping, L"서비스 중지 중", L"Service stopping") \
  X(ServicePaused, L"서비스 일시 중지", L"Service paused") \
  X(ServiceChanging, L"서비스 상태 전환 중", L"Service changing state") \
  /* The picture a stream shows while a security prompt it cannot show is open */ \
  X(SecureNoticeTitle, L"데스크톱에 보안 확인 창이 떠 있어요", L"A security prompt is open on the desktop") \
  X(SecureNoticeLines, L"이 창은 스트리밍 화면에 보여 주지 못했어요.\n연결을 끊고 Console로 열어 응답하세요.", L"It could not be shown in the stream.\nDisconnect, then open Console to answer it.") \
  /* Windows' shutdown notice for a remote power request */ \
  X(PowerMessage, L"Multitap에서 원격으로 요청했어요.", L"Requested remotely from Multitap.") \
  /* The session printer (Settings > Printers) */ \
  X(PrinterComment, L"Multitap 스트리밍 연결 중에만 있는 프린터예요. 인쇄하면 PC의 프린터로 보내요.", L"This printer exists only during a Multitap streaming connection. Printing sends to your PC's printer.") \
  /* Errors Host settings shows from its settings helper */ \
  X(RpcTooLarge, L"설정 요청이 너무 커요.", L"The settings request is too large.") \
  X(RpcOpenInstalled, L"설치된 Host 설정에서 관리자 설정을 여세요.", L"Open the administrator settings from the installed Host settings.") \
  X(RpcElevationCancelled, L"관리자 승인이 취소됐어요.", L"Administrator approval was cancelled.") \
  X(RpcNeedsAdmin, L"관리자 권한으로 Host 설정을 열어 변경하세요.", L"Open Host settings as administrator to change this.") \
  X(RpcServiceUnsupported, L"지원하지 않는 서비스 동작이에요.", L"This service action is not supported.") \
  X(RpcRecoverFailed, L"가상 화면 복구를 완료하지 못했습니다. Proxmox 콘솔에서 호스트 설정을 열어 상태를 확인하세요.", L"Could not finish restoring the virtual display. Open Host settings in the Proxmox console and check its state.") \
  X(RpcUnsupported, L"지원하지 않는 설정 요청이에요.", L"This settings request is not supported.") \
  X(PendingCancelWait, L"이전 연결 요청을 취소한 뒤 서버가 취소를 확인할 때까지 기다려 주세요.", L"Cancel the previous connection request and wait until the server confirms it.") \
  X(PendingCommitted, L"이미 등록이 완료됐습니다. 연결을 바꾸려면 새 서버에 연결을 요청하세요.", L"Registration is already complete. To change it, request a connection to the new server.") \
  X(DisplayHelperMissing, L"가상 화면 구성 파일이 없습니다. 설치 프로그램을 다시 실행하세요.", L"The virtual display configuration file is missing. Run the setup program again.") \
  X(DisplayConfigureFailed, L"가상 화면 구성을 완료하지 못했습니다. Windows를 다시 시작한 뒤 확인하세요.", L"Could not finish configuring the virtual display. Restart Windows, then check again.") \
  /* MultitapHostTool --windows-autologon-uninstall-cleanup (an administrator's command) */ \
  X(AutologonCleanupDone, L"제품 소유 자동 로그인 정리를 확인했어요. 제품 파일 제거는 별도 단계예요.", L"The product's automatic sign-in cleanup is confirmed. Removing the product files is a separate step.") \
  X(AutologonCleanupFailed, L"자동 로그인 정리를 확인하지 못했어요. Host 설정의 Windows 로그인에서 상태를 확인하세요. 복구 기록과 Host 도구를 삭제하지 마세요.", L"Could not confirm the automatic sign-in cleanup. Check its state in Host settings > Windows sign-in. Do not delete the recovery records or the Host tools.")

namespace host::ui_text {
enum class Id : unsigned {
#define MULTITAP_HOST_TEXT_ID(id, ko, en) id,
  MULTITAP_HOST_TEXTS(MULTITAP_HOST_TEXT_ID)
#undef MULTITAP_HOST_TEXT_ID
  Count
};
struct Entry { const char* name; const wchar_t* korean; const wchar_t* english; };
inline constexpr Entry entries[] = {
#define MULTITAP_HOST_TEXT_ENTRY(id, ko, en) {#id, ko, en},
  MULTITAP_HOST_TEXTS(MULTITAP_HOST_TEXT_ENTRY)
#undef MULTITAP_HOST_TEXT_ENTRY
};
static_assert(sizeof(entries) / sizeof(entries[0]) == size_t(Id::Count), "every Host text id has one row");
using multitap::ui::Language;
inline const wchar_t* get(Id id, Language language) {
  const auto& entry = entries[size_t(id)];
  return language == Language::korean ? entry.korean : entry.english;
}
// In this process's language (the signed-in person's, or the one forced).
inline const wchar_t* get(Id id) { return get(id, multitap::ui::processLanguage()); }
inline std::wstring format(Id id, std::initializer_list<std::wstring> arguments, Language language) { return multitap::ui::fill(get(id, language), arguments); }
inline std::wstring format(Id id, std::initializer_list<std::wstring> arguments) { return multitap::ui::fill(get(id), arguments); }
// UTF-8, for an exception or JSON text (runtime_error, the settings helper's answers).
inline std::string utf8(const wchar_t* value) {
  std::string out; if (!value || !*value) return out;
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return out;
  out.resize(size_t(size)); WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr); out.pop_back(); return out;
}
inline std::string utf8(Id id) { return utf8(get(id)); }
}  // namespace host::ui_text
