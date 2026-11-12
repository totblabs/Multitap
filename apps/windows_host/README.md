# Multitap Host

Windows VM(또는 Windows PC) 안에서 실행되는 Multitap 프로그램입니다. 브로커와 연결을 유지하고, 스트리밍 세션의 화면·소리·입력과 장치 공유를 처리합니다. 전체 구조는 [ARCHITECTURE.md](../../ARCHITECTURE.md)에 있습니다.

## 설치

1. 브로커의 다운로드(`/get/host`)에서 받은 `Multitap-Host-<버전>-windows-x64.exe`를 실행하고 관리자 승인을 누릅니다. 설치 파일은 Windows Installer 패키지로 프로그램 파일과 Windows 서비스를 설치한 뒤 Host 설정을 엽니다. CMD나 PowerShell을 직접 실행할 필요는 없습니다.
2. 처음 설치라면 Host 설정이 바로 **가상 모니터 드라이버의 서명을 신뢰할까요?**를 한 번 묻습니다. 동의하면 Sunshine이 서명한 원본 가상 화면 드라이버(인증서 하나를 로컬 컴퓨터의 Root·TrustedPublisher 저장소에 등록)와 WinFsp를 차례로 설치합니다. 취소하거나 실패하면 Host 상태 맨 위에 **설치 마무리가 필요해요** 카드가 남습니다. 설치 위치는 `%ProgramFiles%\Multitap\Host\bin`이고, 기존 등록과 설정은 업데이트해도 그대로 남습니다.
3. Windows를 스스로 재시작하지 않습니다. 드라이버가 재시작을 요구하면 결과 창이 알려 주고, 일반 업데이트는 재시작 없이 서비스만 다시 시작합니다. 원격(자동) 업데이트는 드라이버나 인증서를 바꾸지 않으며, 가상 화면이 없는 Host는 관리자 페이지에 확인 필요로 표시됩니다. 설치 뒤 상태는 `tools\release\host-fresh-install-check.ps1`(읽기 전용)로 확인할 수 있습니다.
4. Host 설정에서 서버 주소를 넣고 **연결 테스트 → 연결 요청**을 누릅니다. 관리자가 관리자 페이지에서 두 화면의 확인 코드를 비교해 승인하면 서비스가 등록을 마칩니다. 처음 연결하는 순서는 [docs/windows-host-quickstart.md](../../docs/windows-host-quickstart.md)에 있습니다.

## 구성

| 실행 파일 | 역할 |
|---|---|
| `MultitapHost.exe --service` | Windows 서비스 `MultitapHost`(LocalSystem, 부팅 때 자동 시작). Windows 로그인 전에도 브로커 연결을 유지하고, Host 자격 증명, 업데이트, 드라이버, 화면 상태를 맡습니다. 로그인한 사용자 세션에 작업자를 띄웁니다. |
| `MultitapHost.exe --worker` | 사용자 세션의 작업자. 소유 가상 화면 캡처, H.264·Opus 인코딩, 키보드·마우스·터치 입력, 클립보드·드라이브·인쇄·USB 채널. 서비스와는 비공개 named pipe로만 이야기하고 Host 자격 증명을 받지 않습니다. |
| `MultitapHost.exe --tray` | 로그인할 때마다 뜨는 트레이 아이콘. 설정 열기와 상태 확인. 닫아도 서비스와 원격 연결은 계속됩니다. |
| `MultitapHostSettings.exe` | Host 설정 창(관리자 승인 필요). 호스트 상태, 서버 연결, Windows 로그인, 가상 화면, 장치, 진단. |
| `MultitapHostTool.exe` | 명령줄 도구. `--diagnostics`는 비밀 없는 상태를, `--test-server https://...`는 읽기 전용 HTTPS 검사 결과를 출력합니다. |

- Host 자격 증명은 `%ProgramData%\Multitap\Host\private\credentials.dpapi`에 머신 DPAPI로 암호화해 두며, SYSTEM과 Administrators만 읽을 수 있습니다.
- **Windows 로그인**에 Windows 계정을 한 번 저장하면 자동 로그인이 켜지고, 스트리밍이 그 계정의 세션을 씁니다.
- Host 서비스가 브로커에 연결되어 있다는 것만으로 스트리밍 준비가 끝난 것은 아닙니다. 설정 창의 각 영역이 실제로 확인한 상태(완료 / 설정 필요 / 확인 필요)를 보여 줍니다.

## 화면

스트리밍은 Multitap가 소유한 가상 화면만 캡처합니다. 클라이언트 모니터마다 하나(1개 또는 2개)를 클라이언트 창 크기에 맞춰 만들고, 스트리밍하는 동안 다른 화면은 끈 뒤 끝나면 저장한 상태로 되돌립니다. 기본 가상 디스플레이는 libvirtualdisplay와 SunshineVirtualDisplayDriver([drivers/display_bridge](../../drivers/display_bridge/README.md)), 예전 방식은 MttVDD([drivers/virtual_display](../../drivers/virtual_display/README.md))입니다. 드라이버 파일은 upstream 원본 그대로이고, Windows 서명 검사를 낮추지 않습니다. 자세한 동작: [docs/display-manager-v2.md](../../docs/display-manager-v2.md)

## 인코더

기본은 x264(GPL, 고정 커밋, 수정 없이 정적 링크)이며, GPU가 있으면 NVENC나 AMF를 쓰고, 실패하면 Microsoft 소프트웨어 인코더로 바꿉니다. 관리자만 쓸 수 있는 `%ProgramData%\Multitap\Host\encoder.json`으로 선택을 바꿀 수 있습니다(예: `{"videoEncoder":"mft"}`). 연결 통계의 `encoder`가 실제로 쓰는 인코더입니다. 자세한 내용: [docs/x264-encoder.md](../../docs/x264-encoder.md)

## 빌드

빌드 준비(Visual Studio Build Tools, Flutter, x264와 NASM, 드라이버와 SDK 받기)는 저장소의 [README.md](../../README.md) "소스에서 빌드"를 따릅니다.

```powershell
powershell -File tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native
```

- `MultitapHost.exe`와 `MultitapHostTool.exe`를 빌드하고 Host C++ 테스트를 실행합니다. x264는 `.tools\x264`(고정 커밋, 수정 없음을 CMake가 확인), NASM은 `.tools\nasm-2.16.03\nasm.exe`를 씁니다.
- Host 설정 창은 `tools\build-portable-windows.ps1 -Variant HostSettings`로 빌드합니다.
- 설치 파일은 `tools\host-installer\reversion-msi.ps1`(이전 버전의 패키지와 설치된 모습 그대로의 파일 폴더로 새 버전 패키지를 만듦)과 `tools\host-installer\build.ps1 -Msi <패키지> -Version <버전>`으로 만듭니다.
- 설치된 `bin\`에는 `THIRD-PARTY-NOTICES.txt`, `COPYING.*`, Host 소스 묶음(`host-native-corresponding-source-<버전>.zip`)과 `native-display-driver\`, `driver\`, `usb-driver\`, `winfsp\`가 들어 있습니다. 외부 구성요소와 라이선스는 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)에 있습니다.
