# Multitap

[English](README.md)

**Proxmox VE용 VDI.** Proxmox VE의 Windows VM(과 Windows PC)을 다른 Windows PC나 Android 기기에서 원격 데스크톱으로 쓰게 해 주는 오픈소스 도구입니다. GPU 없는 VM에서도 CPU H.264 인코딩으로 동작하는 것을 첫 목표로 만들었습니다.

Multitap은 Proxmox VE를 위한 독립 도구입니다. Proxmox Server Solutions GmbH가 만들거나 보증한 제품이 아니며, 제휴 관계도 없습니다.

**보증 없음.** Multitap은 AGPL-3.0에 따라 있는 그대로 제공하며 어떤 보증도 하지 않습니다. 직접 설치하고 운영하면서 생기는 문제(데이터 손실, 보안 사고, 서비스 중단 등)의 책임은 사용자에게 있습니다. 자세한 내용은 [LICENSE](LICENSE) 15·16조에 있습니다.

> 상태: 개인이 취미로 만드는 프로젝트입니다. 실제로 쓰고 있지만 기능과 품질은 계속 바뀝니다. 앱, 관리자 페이지, 설치 스크립트는 한국어와 영어로 나옵니다.

설치와 사용 순서는 [가이드북](docs/guide/README.md)([English](docs/guide/en/README.md))을 보세요. 서버 설치·Host 등록부터 Windows·Android 접속과 문제 해결까지 안내합니다.

## 구성

| 구성요소 | 위치 | 하는 일 |
|---|---|---|
| 브로커/릴레이 | `server/` (Node.js 24, Docker) | 로그인, 계정·데스크톱 배정, Proxmox VM 목록과 전원 관리, 스트리밍 중계(WSS, UDP), 관리자 페이지, 설치 파일 배포와 업데이트 |
| Host | `apps/windows_host/` (C++) | Windows VM 또는 PC 안의 서비스. Multitap 가상 디스플레이를 캡처·인코딩하고 입력·소리·장치 공유를 처리 |
| 클라이언트 | `apps/windows_client/` (Flutter, C++, Kotlin) | Windows와 Android 앱. 배정된 데스크톱 목록과 스트리밍 창. Host 설정 화면도 같은 코드 |
| 공용 네이티브 코드 | `native/` | 영상·UDP 미디어·클립보드·서명 확인 등 Host와 클라이언트가 함께 쓰는 코드 |

모든 연결은 브로커를 거칩니다. 클라이언트와 Host가 같은 LAN에 있을 필요는 없습니다. 구조는 [ARCHITECTURE.md](ARCHITECTURE.md)에 있습니다(영어).

## 주요 기능

- 브로커 중계 스트리밍(WSS, 브로커에서 켜면 손실에 강한 UDP 경로와 자동 대체), H.264(x264, 가능하면 NVENC/AMF), Opus 소리
- VM 안의 가상 디스플레이 1~2개, 클라이언트 창 크기에 맞춘 자동 해상도. VM의 Proxmox 디스플레이 종류와 상관없이 스트리밍
- 관리자가 데스크톱별로 허용하는 클립보드(텍스트·파일), 드라이브, 프린터, USB 장치 공유
- 배정된 VM의 켜기·종료·다시 시작·정지, 로컬 PC의 Wake-on-LAN
- Proxmox 콘솔을 통한 긴급 Console(브라우저, noVNC). 여는 동안 VM의 기본 화면만 켬. 휴대폰·태블릿에서는 메뉴·키보드 버튼, VM의 실제 한/영 상태에 맞춘 한글 입력
- VM을 다른 노드로 옮기거나 새 VM ID로 복원해도 데스크톱이 VM을 따라감(배정 유지). 복제한 VM의 Host는 새로 연결을 요청
- 설치 파일과 업데이트에 프로젝트 자체 Ed25519 서명

## 필요한 것

- **서버(브로커):** 아래 한 줄 설치용 Debian 또는 Ubuntu x86_64 CT나 VM(인터넷 연결, Docker는 함께 설치). Docker Engine과 Compose 플러그인이 있는 다른 linux/amd64 서버는 [Compose 직접 설치](docs/docker-deployment.md)로 됩니다. 밖에서 쓰려면 공유기 포트 포워딩, 또는 도메인과 HTTPS
- **Proxmox VE:** 서버에서 Proxmox API(기본 8006 포트)에 접속할 수 있어야 합니다
- **Host:** Windows 10/11 x64 VM 또는 PC
- **클라이언트:** Windows 10/11 x64, Android 8.0(API 26) 이상
- **빌드 PC:** Windows 10/11 x64. 아래 "소스에서 빌드"를 보세요

## 서버 설치

아래의 `192.0.2.10`과 `vdi.example.com`은 예시입니다.

1. **Linux 준비.** Debian 또는 Ubuntu CT나 VM(x86_64). Proxmox CT라면 **옵션 → 기능 → nesting**(권한 없는 CT는 **keyctl**도)을 켜고 다시 시작합니다.

2. **설치 명령 실행.** root 셸에서:

   ```sh
   curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | MULTITAP_LANG=ko bash
   ```

   root가 아니면 `… | sudo env MULTITAP_LANG=ko bash`. `MULTITAP_LANG`이 없으면 Linux 언어 설정을 따릅니다(`ko`로 시작하면 한국어, 아니면 영어).

   Docker가 없으면 설치하고, `/opt/multitap`(`compose.yaml`, `.env`)을 만들고, `ghcr.io/totblabs/multitap-broker`를 받아 서버를 시작한 뒤 `multitap` 명령을 설치합니다. 중간에 두 가지를 묻고, Enter는 예입니다.
   - **앱과 Host가 이 주소로 접속하게 할까요?** `http://서버IP:18480`, 같은 네트워크용(암호화 없음).
   - **밖에서도(공유기 포트 포워딩) 접속하게 할까요?** 공인 IP를 찾은 때만 묻습니다. `http://공인IP:18480`을 등록하고, 공인 IP가 바뀌면 따라갑니다(10분마다 확인).

   다시 실행해도 안전합니다. 다 설치된 서버는 그대로 두고, 중간에 멈춘 설치는 남은 설정과 데이터로 이어서 설치합니다.

3. **처음 설정.** 마지막 화면에 ① 주소, ② 처음 로그인 `admin` / `admin`, ③ 일회용 설정 코드가 나옵니다(다시 보기: `sudo multitap code`). 브라우저로 주소를 열고 로그인한 뒤 설정 코드를 넣고 새 관리자 아이디와 비밀번호를 정합니다.

4. **앱과 Host가 쓸 주소.** 설치 때 예로 답했다면 `http://서버IP:18480`이 이미 됩니다. 주소를 바꾸려면 관리자 페이지 **서버 접속 설정**에 넣으세요. 서버에서 실행할 `sudo multitap address https://vdi.example.com http://192.0.2.10:18480` 같은 명령이 나옵니다. 밖에서 쓰려면 공유기에서 **TCP 18480**과 **UDP 18481**을 서버로 포트 포워딩합니다. 포워딩하지 않으면 밖에서는 들어오지 않습니다.
   - **HTTP는 암호화하지 않습니다.** 비밀번호·화면·키 입력이 그대로 지나갑니다. 도메인이 있으면 리버스 프록시로 HTTPS를 쓰세요. 프록시는 `http://127.0.0.1:18480`으로 넘기고 `Host`, `X-Forwarded-Proto: https`, WebSocket 헤더를 전달합니다. 같은 서버의 프록시는 Docker 게이트웨이 주소를 `MULTITAP_TRUSTED_PROXIES`에 넣어야 합니다. 자세히: [docs/docker-deployment.md](docs/docker-deployment.md), [가이드 2장](docs/guide/02-서버-설치.md).

5. **Proxmox 연결.** 관리자 페이지 **Proxmox 연결**에서 명령을 복사해 Proxmox 노드의 **Shell**(root)에 붙여 넣습니다. 전용 사용자·역할·API 토큰을 만들고(`VM.Audit`, `VM.PowerMgmt`, `VM.Console`, 게스트 에이전트 읽기. 하드웨어 권한 없음) `MULTITAP-CONNECTION-V1` 블록을 출력합니다. 그 블록을 관리자 페이지에 붙여 넣습니다. 노드 주소는 스스로 찾고, 확인되지 않을 때만 묻습니다. 직접 정하려면 명령 맨 끝에 주소를 붙입니다: `… --lang ko https://pve.example.com:8006`. 명령은 받은 스크립트를 서버가 알려 준 SHA-256과 맞춰 보고, 같을 때만 실행합니다. `--lang ko|en`으로 언어를 고릅니다. [가이드 3장](docs/guide/03-Proxmox-연결.md).

6. **Host 설치와 승인.** 각 Windows VM이나 PC에서 `서버주소/get/host`로 Host를 받아 실행합니다. Host 설정이 열려 설치를 마무리합니다(드라이버 서명에 한 번 동의하면 가상 화면 드라이버와 WinFsp를 설치). 서버 주소를 넣고 연결을 요청한 뒤, 두 화면의 확인 코드를 비교해 관리자 페이지에서 승인합니다. **Windows 로그인**에 Windows 계정을 저장합니다. [가이드 4장](docs/guide/04-Host-설치.md).

7. **사용자.** 관리자 페이지에서 계정을 만들고 데스크톱을 배정합니다. 사용자는 `/get/client`(Windows)나 `/get/android`에서 앱을 받아 서버 주소로 로그인합니다.

### `multitap` 명령

| 명령 | 하는 일 |
|---|---|
| `sudo multitap status` | 실행 상태, 버전, 주소 |
| `sudo multitap logs` | 서버 기록 (Ctrl+C로 멈춤) |
| `sudo multitap code` | 처음 설정 코드 다시 보기 |
| `sudo multitap update [1.2.3]` | 백업한 뒤 새 버전(또는 그 버전)으로 올리기. 새 버전이 응답하지 않으면 되돌림 |
| `sudo multitap backup` | 서버 데이터 전체를 `/opt/multitap/backups`에 복사 |
| `sudo multitap restore <파일> [--yes]` | 백업 되돌리기: 관리자 페이지 백업 파일(`.mtbk`, 백업 암호를 물음) 또는 `multitap backup` 복사본. 파일을 확인하고 먼저 물어봄. 안전 백업을 남기고, 실패하면 그것으로 되돌림 |
| `sudo multitap address <주소>…` | 앱과 Host가 쓰는 주소(같은 네트워크는 `http://…`, 프록시 뒤는 `https://…`). 주소 없이: 지금 주소 보기 |
| `sudo multitap public-ip [on\|off]` | 공인 IP를 HTTP 주소로 등록하고 바뀌면 따라가기(10분마다). 없이: 지금 상태 |
| `sudo multitap restart` · `stop` · `start` | 다시 시작·멈춤·시작 |
| `sudo multitap uninstall` | 서버를 멈추고 지우기. 데이터는 남음 |

명령은 설치 때의 언어로 답합니다(`/opt/multitap/.env`의 `MULTITAP_LANG`). `MULTITAP_LANG=ko|en`을 붙이면 그 언어로 답합니다.

**UDP 미디어:** 브로커는 UDP 18481에서 미디어를 중계하고 Compose가 이 포트를 게시합니다. 방화벽에서 열고 공유기에서 전달한 뒤, `/opt/multitap/.env`에 `MULTITAP_UDP_HOST_RELAYS`, `MULTITAP_UDP_CLIENT_RELAYS`(예: `vdi.example.com:18481`)와 `MULTITAP_UDP_MEDIA=on`을 넣고 `sudo multitap start`를 실행합니다. 이 설정이 없거나 UDP가 막혀 있으면 연결은 WebSocket으로 그대로 동작합니다.

**다운로드 번들:** 공개 이미지에는 Windows 클라이언트·Host 설치 파일과 Android APK(3개 ABI)가 들어 있고, 첫 시작 때 데이터 볼륨으로 복사되어 `/get/client`, `/get/host`, `/get/android`로 나갑니다. 이미지를 소스에서 빌드하려면 `docker compose -f compose.yaml -f compose.build.yaml up -d --build`를 씁니다. 저장소의 `server/release-bundle/`은 빈 번들이므로 릴리스의 번들 압축 파일을 풀어 두거나 `MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=true`를 넣습니다([docs/docker-deployment.md](docs/docker-deployment.md)). 릴리스 순서: [docs/releasing.md](docs/releasing.md). 관리자 안내: [가이드 5장](docs/guide/05-사용자와-VM-배정.md)

## 소스에서 빌드

빌드 스크립트는 Windows PowerShell 5.1에서 실행합니다. 도구와 외부 파일은 저장소 안의 `.tools/`(git 무시)에 둡니다. 외부 파일은 모두 고정된 버전과 SHA-256으로 확인합니다.

**공통 준비**

1. Visual Studio 2022 Build Tools(C++): `https://aka.ms/vs/17/release/vs_buildtools.exe`를 `.tools\downloads\vs_BuildTools.exe`로 받고 `Install-WindowsBuildTools.cmd`를 실행합니다(`.tools\visualstudio-buildtools`에 설치).
2. Flutter: `powershell -File tools\setup-flutter.ps1`(Flutter 3.47.7을 `.tools\flutter`에 설치).
3. Windows 클라이언트 설치 파일을 만들려면 NSIS 3.13 zip을 `.tools\nsis-download\nsis-3.13.zip`에 두고 같은 폴더에 풀어 둡니다. 7-Zip의 `7z.exe`도 `.tools\7zip\`에 둡니다. `tools/build-windows-installer.ps1`이 NSIS 해시를 확인하고, 만든 설치 파일을 7-Zip으로 다시 풀어 검사합니다.

**Windows 클라이언트**

```powershell
Build-Client.cmd
powershell -File tools\build-windows-installer.ps1 -Product client -Version <x.y.z> -Payload apps\windows_client\build\windows\x64\runner\Release
```

**Android 클라이언트:** JDK 21과 Android SDK 36을 `tools/android-workspace.ps1`이 가리키는 `.tools` 경로에 두고 `powershell -File tools\build-android.ps1 -Version <build.gradle.kts의 versionName>`을 실행합니다. 처음에는 `-CreatePreviewKey`를 붙이면 자신의 서명 키가 `.local\android`에 만들어집니다. Gradle 래퍼(`apps/windows_client/android/gradlew`, Flutter가 만드는 표준 래퍼)는 저장소에 들어 있습니다.

**Host**

Host 설치 파일은 EXE 안에 Windows Installer 패키지가 들어 있고(`apps/windows_host/installer/host_installer.cpp`), `tools\host-installer\`의 스크립트로 만듭니다. x264 고정 커밋과 NASM 준비부터 합니다([apps/windows_host/third_party/x264/README.md](apps/windows_host/third_party/x264/README.md)). 이 스크립트들은 `.tools`에 준비된 도구를 읽습니다. 경로는 각 스크립트 맨 위에 있습니다(MSVC 환경 스크립트 `.tools\msvc-portable\env.ps1`, CMake 3.31.8, Ninja, libvirtualdisplay SDK, x264, NASM, Opus 1.6.1 tarball). 이 가운데 컴파일러 부분을 준비해 주는 스크립트는 없습니다.

1. `powershell -File tools\fetch-libvirtualdisplay.ps1`로 libvirtualdisplay SDK 소스와 서명된 드라이버를 받습니다.
2. `powershell -File tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native`가 `MultitapHost.exe`와 `MultitapHostTool.exe`를 빌드하고 C++ 테스트를 실행합니다.
3. `powershell -File tools\build-portable-windows.ps1 -Variant HostSettings`가 Host 설정(Host 설정 모드의 클라이언트)을 빌드합니다.
4. `tools\host-installer\reversion-msi.ps1`이 이전 릴리스의 패키지와, 설치된 Host와 같은 구조의 payload 폴더로 새 버전의 패키지를 만들고, `tools\host-installer\build.ps1 -Msi <패키지> -Version <x.y.z>`가 그것을 `Multitap-Host-<x.y.z>-windows-x64.exe`로 묶습니다. 패키지는 항상 바로 앞 패키지에서 만들고 그 첫 패키지는 이 저장소에 없으므로, 새로 받은 저장소만으로는 이 단계를 끝낼 수 없습니다.

payload의 드라이버 파일은 고정된 다운로드에서 오고, 모두 해시와 서명을 확인합니다: `tools\build-virtual-display.ps1`(MttVDD와 helper, `bin\driver`), `tools\fetch-libvirtualdisplay.ps1`(`bin\native-display-driver`), `tools\fetch-usb-drivers.ps1`(usbip-win2, `bin\usb-driver`), `tools\fetch-winfsp.ps1`(WinFsp, `bin\winfsp`).

**브로커:** `server/`에서 `npm ci`와 `npm test`(Node.js 24). Docker 이미지를 소스에서 빌드하려면 `docker compose -f compose.yaml -f compose.build.yaml build`.

테스트를 돌리는 방법은 [CONTRIBUTING.md](CONTRIBUTING.md)에 있습니다.

**서명 키:** 공식 릴리스는 이 프로젝트의 Ed25519 키로 서명하고, 클라이언트와 Host는 소스에 고정된 공개키(`native/windows_admin/release_signature.hpp`)로 확인합니다. 직접 빌드한 판을 배포하려면 자신의 키를 만들고(`tools/windows-release-signature.mjs`, 서버 업데이트용 `tools/create-server-update-key.mjs`) 고정 공개키를 바꿔야 합니다. 개인키는 저장소에 넣지 마세요.

## 라이선스

Multitap은 [GNU AGPL v3](LICENSE)만으로 배포합니다(SPDX: `AGPL-3.0-only`, 이후 버전 선택 조항 없음). 수정한 버전을 네트워크 서비스로 운영하면 그 서비스의 사용자에게 소스를 제공해야 합니다(AGPL-3.0 13조). "Multitap"이라는 이름과 로고를 수정한 버전에 원본인 것처럼 보이게 쓰는 것은 허락하지 않습니다.

함께 쓰는 외부 구성요소와 각 라이선스, Host의 대응 소스는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)에 있습니다. Host 배포판에는 GPL인 x264가 정적으로 들어가며, 패키지에 x264 소스와 고지가 함께 들어 있습니다.

보안 문제는 공개 이슈 대신 [SECURITY.md](SECURITY.md)의 방법으로 알려 주세요. 기여 방법은 [CONTRIBUTING.md](CONTRIBUTING.md)에 있습니다.
