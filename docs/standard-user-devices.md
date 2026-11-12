# A standard Windows user and the device features

A stream works when the Windows user signed in on the
desktop is a standard (non-administrator) account: the picture
([display-manager-v2.md](display-manager-v2.md#which-windows-user)) and the
input ([system-input.md](system-input.md)). This page is the same question for
everything else a session carries: the clipboard, shared folders, USB devices,
printing, sound, the preview picture and Host settings. It was answered by
reading the code; **nothing here has run with a real standard
account yet**. The checklist at the end is how to find out on a real VM.

## The short answer

None of these features needs the signed-in user to be an administrator, and
none needed a new request to the service. The reason is how they were built:

- What needs rights is done by the service (LocalSystem): the printer queue and
  its port, the USB controller and its driver, the WebClient service, the
  loopback servers.
- What belongs to the user is done by a helper the service starts **with that
  user's ordinary token** (`WTSQueryUserToken`): the drive letter, the default
  printer, the preview picture. For an administrator that token is the filtered
  one, without administrator rights, so these helpers have always run the way a
  standard user's do.
- Only two of the features live in the stream worker, the one process that is
  elevated for an administrator and is not for a standard user: the clipboard
  and sound. Neither uses a privilege or a file outside the user's own profile.

Every list of who may use what names the user's own account, never a group, so
the lists are the same for both kinds of account.

## Feature by feature

| Feature | Step | Who does it | Standard user |
|---|---|---|---|
| Clipboard | Watch, read and write the clipboard (text, images, rich text) | worker, own session | Works. The clipboard is shared between integrity levels of one desktop; no privilege. |
| | Receive files (store under `%LOCALAPPDATA%\Multitap\ClipboardTransfers`, protected list: the account, SYSTEM, Administrators) | worker | Works: the user's own profile (`native/windows_clipboard/storage.hpp`, the code the unelevated Windows client runs too). |
| | Send files the user copied | worker, with the user's own rights | Works. A file the user cannot read is not sent (`clipboard_file_unavailable`), as at the console. |
| | Diagnostics (`hostAction 253`) | worker → service over the private pipe (`clip.diag`) | Works: the worker never wrote the journal itself. |
| Shared folders | WinFsp volume and drive letter | `MultitapHost.exe --drive-fs`, user's ordinary token | Works: the letter is defined in the user's logon session, the volume's list names the account. |
| | WebDAV fallback (WebClient service, size caps) | service | Works. |
| | WebDAV drive letter, label, opening the folder | `--drive-mount`, user's ordinary token | Works. |
| | WinFsp itself | Host setup, or Host settings → 장치 | **Administrator, once**: it is a driver. Without it the WebDAV drive is used. |
| USB | Controller driver (usbip-win2) install, protection, attach, detach | service | Works: no prompt, the driver carries Microsoft's signature. The controller is closed to every user process. |
| | The device's own driver in the desktop | Windows | In-box classes (storage, HID, smart card, serial) install by themselves. A device Windows has no driver for: **administrator, once** (below). |
| Printing | Queue, port, document storage, removal | service | Works. Only the session's account may print (`printerSecurityDescriptor`). |
| | Default printer claim and restore | `--print-default`, user's ordinary token | Works: a per-user setting. |
| | A stopped Print Spooler | Host settings → 장치 | **Administrator**: starting a service. The client already says so. |
| Sound | WASAPI loopback of the default output | worker | Works: no privilege. A desktop without an audio device has no sound for anyone. |
| Preview picture | One capture of the unlocked desktop | `--thumbnail-capture`, user's ordinary token | Works. |
| Host settings | The whole window | `MultitapHostSettings.exe` (requires an administrator) | **Administrator**: it changes the service, drivers and the server connection. A standard user gets Windows' own prompt for an administrator's name and password; the stream shows it and takes the answer as it does any UAC prompt ([system-input.md](system-input.md); with the prompt on the secure desktop that path has not run for real yet). The tray icon's status line needs nothing. |

## Limits a standard user has, and why

They all follow from one rule: the redirected devices belong to the account
that is signed in, and to nobody else.

- **Programs started with another account's rights.** A standard user who
  answers a UAC prompt with an administrator's password runs that program as
  the administrator's account. It is a different account, so it has no drive
  letter for the shared folders (drive letters belong to a logon), may not
  print to the Multitap printer (administrators manage that queue but have no
  Print entry, so no other account prints into this user's stream), and cannot
  paste files offered from the PC: those are virtual files served by the
  worker over COM, and Windows lets only the worker's own account and SYSTEM
  call it ("only the server principal and local system are allowed to call the
  server", learn.microsoft.com `com/defaultaccesspermission`, read
  2026-11-10). Text and images paste there as usual. An administrator's own
  elevated programs have the first of these limits too (documented in
  [drive-redirection.md](drive-redirection.md)).
- **A USB device Windows has no driver for.** Installing a driver needs an
  administrator of the desktop. The device stays attached (it works as soon as
  the driver is there) and the Host records what Windows says about it:
  `hostAction 250`, stage 28, `error` = Windows' problem code (28: no driver,
  52: a driver Windows does not accept), once per device, after the code has
  stayed for three polls and ten seconds. The
  client still shows the device as connected; telling the user there needs a
  new `usb-v1` message.
- **WinFsp, a stopped spooler, another product's USB controller.** Each is a
  machine change in Host settings, which asks for an administrator.

Nothing was added to what the SYSTEM input path or the secure-desktop helpers
do, and no Windows security setting is read or changed.

## What the tests show

`apps/windows_host/src/standard_user_access_test.cpp`
(`MultitapHostStandardUserAccessTest`) asks Windows (`AccessCheck`) about every
security descriptor these features use, for a stand-in of a standard user and
of a second account (this process's token with its powerful groups, and then
its own account, switched off):

| Object | The signed-in standard user | Another account |
|---|---|---|
| `ProgramData\Multitap\Host` | read, nothing else | read, nothing else |
| `...\Host\private` (credential, display state, print documents) | nothing | nothing |
| service ↔ worker pipe | read and write, cannot change its permissions | nothing |
| Multitap printer | Print, cannot manage | nothing |
| shared folder volume (WinFsp) | everything | nothing |
| USB controller | nothing | nothing |
| clipboard file store (a real one, made without administrator rights) | everything | nothing |

It runs without administrator rights (25 checks). The clipboard's
own scenario test (`MultitapHostClipboardTest`) was not run with it: it needs
its own window station, which takes an administrator, or it replaces the
clipboard of the desktop it runs on.

`usb_redirection_test.cpp` covers the USB driver record: written once, not before
Windows had time to install, not for a device that starts, not for a failed
start (problem 10, which ends the attachment as before).

## 실제 VM에서 확인하기 (일반 계정)

일반 Windows 계정으로 로그인된 데스크톱에 연결한 뒤 차례로 해 봐요. 실패하면
관리자 페이지의 데스크톱 → VM 상세 → 활동에서 오른쪽 기록을 찾아요.

| 확인 | 하는 일 | 잘 되면 | 안 되면 볼 기록 |
|---|---|---|---|
| 글자 복사 (PC → 데스크톱) | PC 메모장에서 복사, 데스크톱 메모장에 Ctrl+V | 같은 글자가 붙어요 | `hostAction 253` 단계 5(받기 실패) |
| 글자 복사 (데스크톱 → PC) | 반대로 | 같은 글자가 붙어요 | `hostAction 253` 단계 6(보내기 실패), 10(알리지 않음) |
| 그림 복사 | 캡처 도구로 찍어 양쪽으로 붙여넣기 | 그림이 붙어요 | `hostAction 253` 단계 5·6, `format` 4 |
| 파일 복사 (PC → 데스크톱) | PC 탐색기에서 100MB쯤 되는 파일을 복사해 데스크톱 바탕 화면에 붙여넣기 | 진행 표시 뒤 파일이 생기고 열려요 | `hostAction 253` 단계 5·9, `cause` |
| 파일 복사 (데스크톱 → PC) | 데스크톱의 내 문서에서 복사해 PC에 붙여넣기 | 파일이 생겨요 | `hostAction 253` 단계 6 |
| 폴더 공유 | 툴바 폴더·USB에서 PC 폴더 하나 공유 | 데스크톱 탐색기에 "이름 (PC)" 드라이브가 생겨요 | `hostAction 247` 단계 13(WinFsp 됨)·14·15(WebDAV로 바뀜)·3(실패), `cause` |
| 큰 파일 열기·저장 | 공유 폴더의 1GB 넘는 파일을 열고, 데스크톱에서 만든 큰 파일을 공유 폴더에 저장 | 끝까지 복사되고 PC에서 열려요 | `hostAction 247` 단계 12·16 |
| USB 저장 장치 | 툴바 폴더·USB에서 USB 메모리 연결 | 데스크톱 탐색기에 드라이브가 생기고 파일이 열려요. 끊으면 PC로 돌아와요 | `hostAction 250` 단계 5·6·8(드라이버·보호), 12(연결 실패), 28(`error` 10: 시작 실패) |
| USB HID 장치 | 바코드 스캐너나 서명 패드처럼 키보드·마우스가 아닌 HID 장치 연결 | 데스크톱 장치 관리자에 나타나고 동작해요 | `hostAction 250` 단계 28(`error` 28: 드라이버 없음 → 관리자가 데스크톱에 드라이버를 한 번 설치) |
| 인쇄 | 데스크톱 메모장에서 인쇄(프린터를 고르지 않고) | "Multitap 프린터 (PC)"가 기본 프린터이고, PC에 인쇄 창이 떠요 | `hostAction 242` 단계 16·17(프린터 못 만듦), 18(사용자 확인 못 함), 30(기본 프린터, `error`·`cause`) |
| 인쇄 뒤 | 연결을 끊고 데스크톱의 프린터 목록 보기 | Multitap 프린터가 없고 기본 프린터가 원래대로예요 | `hostAction 242` 단계 2·28·29 |
| 소리 | 데스크톱에서 동영상 재생 | PC에서 소리가 나요 | Host 설정 → 장치 → 소리(출력 장치 없음이면 Proxmox에서 오디오 장치 추가), 클라이언트 통계의 `audioCapture` |
| 미리보기 | 연결을 끊고 PC 목록 화면에서 1분 기다리기 | 카드에 데스크톱 화면이 보여요 | `hostAction 280` (단계 0 실패, 3 잠김) |
| 다른 계정 | 데스크톱에 다른 일반 계정으로 로그인한 세션을 하나 더 두고(사용자 전환) 위 폴더·프린터가 그쪽에서 보이는지 보기 | 드라이브가 없고 프린터로 인쇄가 거부돼요 | 기록 없음. 보이거나 인쇄되면 알려 주세요 |
| 관리자 권한 프로그램 | UAC 창에 관리자 암호를 넣어 프로그램을 열고 글자 붙여넣기 | 글자는 붙어요. 그 프로그램에서 공유 드라이브·Multitap 프린터·PC 파일 붙여넣기는 안 돼요(위 한계) | 기록 없음 |
| Host 설정 | 트레이 아이콘 → Host 설정 열기 | 관리자 이름과 암호를 묻는 창이 스트림에 보이고, 넣으면 설정이 열려요 | 기록 없음 |
