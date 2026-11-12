# Multitap 클라이언트

Windows 앱, Android 앱, Host 설정 창을 하나의 Flutter 코드로 만든다. 로그인한 계정에 서버가 할당한 데스크톱만 보여 주며, VM 등록·편집·삭제와 직접 연결은 없다(관리는 브로커의 관리자 콘솔에서 한다).

## 구성

| 위치 | 내용 |
|---|---|
| `lib/main.dart`, `lib/android_main.dart` | Windows·Android 진입점 |
| `lib/ui/`, `lib/data/`, `lib/models/` | 화면, 브로커 API와 세션, 스트림 옵션 등 |
| `lib/android/` | Android 전용 화면(같은 카드 디자인과 같은 툴바 항목) |
| `windows/runner/` | C++: H.264 디코딩(Media Foundation), Opus, 키보드 훅, 클립보드, 공유 폴더, 인쇄, USB(UsbDk), UDP 미디어, 다중 모니터 창, WebView2 비상 콘솔, 자격 증명 저장 |
| `android/` | Kotlin: MediaCodec 디코딩, Opus, UDP, 클립보드, 인쇄, TV 지원 |
| `test/`, `integration_test/` | Flutter 테스트, `test/native/`는 C++ 스모크 테스트 |

Host 설정 창(`MultitapHostSettings.exe`)은 이 클라이언트의 Windows 빌드를 Host 패키지에 복사하고 실행 수준을 관리자 권한(requireAdministrator)으로 바꾼 것이다.

## 동작

- **로그인:** 첫 실행에는 서버 주소가 비어 있다. HTTPS 연결 테스트를 통과한 주소만 저장한다. 아이디는 서버별로 기억하고, 비밀번호는 "비밀번호 기억"을 고른 경우에만 현재 사용자의 Windows 자격 증명 관리자에 저장한다. 세션 토큰은 메모리에만 둔다.
- **목록:** 할당된 데스크톱 카드와 실제 전원 상태를 보여 준다. 연결은 카드의 연결 버튼(또는 포커스된 카드에서 Enter)으로만 시작한다. ⏻ 버튼은 켜기·종료·다시 시작·정지(즉시 전원 차단, 확인 필요)를, ⋯ 메뉴는 그 밖의 항목을 연다. 업데이트가 있으면 배지를 보여 준다.
- **스트리밍 창:** 데스크톱마다 별도 창으로 열리고 목록은 그대로 남는다. 창을 닫으면 그 연결만 끊는다. 새 연결은 60 fps, 자동 전송률로 시작한다([스트림 기본값](../../docs/stream-default-profile.md)).
- **해상도:** 창 크기에 맞춰 VM의 Multitap 가상 디스플레이 해상도가 실제로 바뀐다. 배율은 100–200 %(25 % 단위) 중에서 고르고, 현재 실제 배율을 보여 준다.
- **툴바:** 평소에는 위쪽 가운데의 작은 손잡이로 접혀 있고 클릭해야 열린다. 항목은 연결 품질 · 화면 · 배율 · 설정 · 공유 폴더 · USB 장치 · 마우스 잠금 · 최소화 · 사용안내 · 연결 끊기 순서이며, Android의 ⋯ 메뉴도 같은 이름과 순서를 쓴다.
- **입력:** 원격 화면을 클릭하면 키보드·마우스가 원격으로 간다. Windows 키와 일반 단축키도 원격에서 처리한다. 포커스를 잃거나 연결이 끊기면 눌린 입력을 해제한다. **Ctrl+Alt+Shift+F12**는 키보드를 로컬로 돌려받는다. Ctrl+Alt+Del과 UAC 보안 데스크톱은 지원하지 않는다.
- **마우스 잠금:** 연결할 때마다 꺼져 있다. 켜면 해제 방법을 팝업으로 알려 주고 마우스를 VM 안에 잠가 상대 이동을 보낸다(권한이 있으면 가상 USB 마우스). **Ctrl+Alt+Shift+Z**로 푼다. 메뉴·대화상자·창 비활성화·연결 종료 때도 먼저 풀린다.
- **장치 공유:** 클립보드(텍스트·이미지·파일, 붙여넣을 때 전송), 공유 폴더, 프린터, USB 장치는 관리자가 계정×VM마다 허용한 것만 동작한다.
- **화면 절전:** 원격 창이 열려 있는 동안만 Windows 화면·시스템 절전 방지를 요청하고, 연결이 끝나면 해제한다.
- **권한:** 클라이언트는 일반 사용자 권한(asInvoker)으로 실행한다. UsbDk 드라이버나 업데이트 설치처럼 꼭 필요한 작업에서만 관리자 승인을 요청한다.

## 빌드와 테스트

준비는 저장소 루트의 [README](../../README.ko.md#소스에서-빌드)를 따른다.

```powershell
Build-Client.cmd           # 저장소 루트: apps\windows_client\build\windows\x64\runner\Release
Install-Client.cmd         # 로컬 빌드를 %LOCALAPPDATA%\Multitap\Client\<버전>에 설치하고 실행
powershell -File tools\build-windows-installer.ps1 -Product client -Version <x.y.z> -Payload apps\windows_client\build\windows\x64\runner\Release
```

`apps\windows_client`에서 `..\..\flutter.cmd analyze`와 `..\..\flutter.cmd test`로 검사한다. Android 빌드는 `tools\build-android.ps1`을 쓴다.
