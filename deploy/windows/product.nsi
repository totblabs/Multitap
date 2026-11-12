; Build-time definitions: PAYLOAD, OUTPUT, SETUP, PRODUCT, VERSION, ADMIN.
Unicode true
!include "FileFunc.nsh"
RequestExecutionLevel ${ADMIN}
; The Windows client installer. (The Host installer is built by tools/host-installer.)
!define DISPLAY_PRODUCT "Multitap"
!define SETUP_ICON "..\..\apps\windows_host\resources\client_setup.ico"
Name "${DISPLAY_PRODUCT} ${VERSION}"
OutFile "${OUTPUT}"
Icon "${SETUP_ICON}"
SetCompressor /SOLID lzma
CRCCheck force
SilentInstall silent
AutoCloseWindow true
ShowInstDetails nevershow
; Korean and English (docs/i18n.md). NSIS picks the language of the Windows display
; language when it is one of these, else the first one loaded: English.
LoadLanguageFile "${NSISDIR}\Contrib\Language files\English.nlf"
LoadLanguageFile "${NSISDIR}\Contrib\Language files\Korean.nlf"
LangString ExtractFailed ${LANG_KOREAN} "설치 파일을 풀지 못했어요.$\r$\n디스크 여유 공간과 Windows 보안의 보호 기록을 확인하세요."
LangString ExtractFailed ${LANG_ENGLISH} "Could not unpack the setup files.$\r$\nCheck the free disk space and the protection history of Windows Security."
LangString SetupMissing ${LANG_KOREAN} "설치 실행 파일을 찾지 못했어요.$\r$\nWindows 보안의 보호 기록을 확인하세요."
LangString SetupMissing ${LANG_ENGLISH} "The setup program was not found.$\r$\nCheck the protection history of Windows Security."
LangString LaunchFailed ${LANG_KOREAN} "설치 프로그램을 실행하지 못했어요.$\r$\nWindows 보안의 보호 기록을 확인하세요."
LangString LaunchFailed ${LANG_ENGLISH} "Could not start the setup program.$\r$\nCheck the protection history of Windows Security."
LangString CodeLabel ${LANG_KOREAN} "확인 코드"
LangString CodeLabel ${LANG_ENGLISH} "Code"
; The version resource keeps its one Korean block, as before: an English block sorts first
; (1033 < 1042) and readers that take the first block (e.g. .NET) then show Korean users English.
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1042 "ProductName" "${DISPLAY_PRODUCT}"
VIAddVersionKey /LANG=1042 "FileDescription" "${DISPLAY_PRODUCT} 설치"
VIAddVersionKey /LANG=1042 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1042 "LegalCopyright" "Multitap"
Section
  ${GetParameters} $1
  ClearErrors
  InitPluginsDir
  SetOutPath "$PLUGINSDIR\payload"
  IfErrors extract_failed
  ClearErrors
  File /r "${PAYLOAD}\*"
  IfErrors extract_failed
  IfFileExists "$PLUGINSDIR\payload\${SETUP}" 0 setup_missing
  ; /MultitapManual: an app chose "install with the installer" for a verified
  ; staged copy: the ordinary graphical installer (Install click, every
  ; confirmation), never the staged-copy update recognition.
  ClearErrors
  ${GetOptions} $1 "/MultitapManual" $2
  IfErrors +3
  ExecWait '"$PLUGINSDIR\payload\${SETUP}" --manual' $0
  Goto launched
  ClearErrors
  ${GetOptions} $1 "/MultitapUpdate" $2
  IfErrors manual
  ExecWait '"$PLUGINSDIR\payload\${SETUP}" --update' $0
  Goto launched
manual:
  ExecWait '"$PLUGINSDIR\payload\${SETUP}"' $0
launched:
  IfErrors launch_failed
  SetErrorLevel $0
  Goto done
extract_failed:
  StrCpy $4 "$(ExtractFailed)"
  StrCpy $5 "payload_extract_failed"
  Goto failed
setup_missing:
  StrCpy $4 "$(SetupMissing)"
  StrCpy $5 "setup_missing"
  Goto failed
launch_failed:
  StrCpy $4 "$(LaunchFailed)"
  StrCpy $5 "setup_launch_failed"
failed:
  MessageBox MB_OK|MB_ICONSTOP "$4$\r$\n$(CodeLabel): $5"
  SetErrorLevel 1
done:
SectionEnd
