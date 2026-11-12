# Build from source

[한국어](#소스에서-빌드)

The server needs Node.js 24, or Docker for its image. The apps and the Host build on Windows 10 or 11 x64: the build scripts run in Windows PowerShell 5.1, and tools and external files live in `.tools/` inside the repository (ignored by git). Every external file is checked against a pinned version and SHA-256.

**What a fresh clone cannot build today:** the Host installer. Each Host package is made from the one before it, and that first package is not in this repository (step 4 under Host). The Host's programs and their C++ tests do build, but their scripts read a compiler layout in `.tools` that no script sets up.

## Server

`npm ci` and `npm test` in `server/` (Node.js 24).

To build the Docker image from source instead of pulling it:

```sh
docker compose -f compose.yaml -f compose.build.yaml up -d --build
```

The published image carries the Windows client and Host installers and the Android APKs (three ABIs); they are copied to the data volume on first start and served at `/get/client`, `/get/host` and `/get/android`. The repository's `server/release-bundle/` is an empty bundle, so unpack a release's `multitap-release-bundle-<version>.tar.gz` there, or set `MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=true` for an image without downloads. The details are in [docker-deployment.md](docker-deployment.md) (Korean, with an English summary at the end).

## Common setup on Windows

1. Visual Studio 2022 Build Tools (C++): download `https://aka.ms/vs/17/release/vs_buildtools.exe` to `.tools\downloads\vs_BuildTools.exe` and run `Install-WindowsBuildTools.cmd` (installs into `.tools\visualstudio-buildtools`).
2. Flutter: `powershell -File tools\setup-flutter.ps1` (installs Flutter 3.47.7 into `.tools\flutter`).
3. For the Windows client installer, put the NSIS 3.13 zip at `.tools\nsis-download\nsis-3.13.zip` and unpack it in the same folder, and put 7-Zip's `7z.exe` in `.tools\7zip\`. `tools/build-windows-installer.ps1` checks the NSIS hash and unpacks the finished installer again with 7-Zip to inspect it.

## Windows client

```powershell
Build-Client.cmd
powershell -File tools\build-windows-installer.ps1 -Product client -Version <x.y.z> -Payload apps\windows_client\build\windows\x64\runner\Release
```

## Android client

Put JDK 21 and Android SDK 36 in the `.tools` paths named by `tools/android-workspace.ps1` and run `powershell -File tools\build-android.ps1 -Version <versionName from build.gradle.kts>`. The first time, `-CreatePreviewKey` creates your own signing key in `.local\android`. The Gradle wrapper (`apps/windows_client/android/gradlew`, the standard wrapper Flutter generates) is in the repository.

## Host

The Host installer is a Windows Installer package inside an EXE (`apps/windows_host/installer/host_installer.cpp`), built by the scripts in `tools\host-installer\`. The build starts with the pinned x264 commit and NASM ([apps/windows_host/third_party/x264/README.md](../apps/windows_host/third_party/x264/README.md)). The scripts read a prepared toolchain from `.tools`; the paths are at the top of each script (an MSVC environment script `.tools\msvc-portable\env.ps1`, CMake 3.31.8, Ninja, the libvirtualdisplay SDK, x264, NASM, the Opus 1.6.1 tarball). No script sets up the compiler part of that layout.

1. `powershell -File tools\fetch-libvirtualdisplay.ps1` fetches the libvirtualdisplay SDK source and the signed driver.
2. `powershell -File tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native` builds `MultitapHost.exe` and `MultitapHostTool.exe` and runs their C++ tests.
3. `powershell -File tools\build-portable-windows.ps1 -Variant HostSettings` builds Host settings (the client in Host-settings mode).
4. `tools\host-installer\reversion-msi.ps1` makes the package of a new version from the previous release's package and a payload folder laid out like the installed Host, and `tools\host-installer\build.ps1 -Msi <package> -Version <x.y.z>` wraps it into `Multitap-Host-<x.y.z>-windows-x64.exe`. Every package is made from the one before it, and that first package is not in this repository, so a fresh clone alone cannot finish this step.

The driver files of the payload come from pinned downloads, each checked by hash and signature: `tools\build-virtual-display.ps1` (MttVDD and its helper, `bin\driver`), `tools\fetch-libvirtualdisplay.ps1` (`bin\native-display-driver`), `tools\fetch-usb-drivers.ps1` (usbip-win2, `bin\usb-driver`) and `tools\fetch-winfsp.ps1` (WinFsp, `bin\winfsp`).

## Tests

How to run the tests of each part is in [CONTRIBUTING.md](../CONTRIBUTING.md#tests).

## Signing keys

Official releases are signed with this project's Ed25519 keys, and the clients and the Host check them against the public key pinned in the source (`native/windows_admin/release_signature.hpp`). To distribute your own builds, create your own keys (`tools/windows-release-signature.mjs`; for server updates `tools/create-server-update-key.mjs`) and replace the pinned public key. Never put a private key in the repository. The places a fork changes are listed in [releasing.md](releasing.md#once).

---

## 소스에서 빌드

[English](#build-from-source)

서버는 Node.js 24가 있으면 되고, 이미지는 Docker로 빌드합니다. 앱과 Host는 Windows 10·11 x64에서 빌드합니다. 빌드 스크립트는 Windows PowerShell 5.1에서 실행하고, 도구와 외부 파일은 저장소 안의 `.tools/`(git 무시)에 둡니다. 외부 파일은 모두 고정된 버전과 SHA-256으로 확인합니다.

**새로 받은 저장소만으로 지금 빌드할 수 없는 것:** Host 설치 파일. Host 패키지는 항상 바로 앞 패키지에서 만드는데 그 첫 패키지가 이 저장소에 없습니다(Host의 4번). Host 프로그램과 C++ 테스트는 빌드되지만, 그 스크립트가 읽는 `.tools`의 컴파일러 구성을 준비해 주는 스크립트는 없습니다.

### 서버

`server/`에서 `npm ci`와 `npm test`(Node.js 24).

Docker 이미지를 받지 않고 소스에서 빌드하려면:

```sh
docker compose -f compose.yaml -f compose.build.yaml up -d --build
```

공개 이미지에는 Windows 클라이언트·Host 설치 파일과 Android APK(3개 ABI)가 들어 있고, 첫 시작 때 데이터 볼륨으로 복사되어 `/get/client`, `/get/host`, `/get/android`로 나갑니다. 저장소의 `server/release-bundle/`은 빈 번들이므로 릴리스의 `multitap-release-bundle-<버전>.tar.gz`를 풀어 두거나, 다운로드 없는 이미지를 만들려면 `MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=true`를 넣습니다. 자세한 내용은 [docker-deployment.md](docker-deployment.md)에 있습니다.

### Windows 공통 준비

1. Visual Studio 2022 Build Tools(C++): `https://aka.ms/vs/17/release/vs_buildtools.exe`를 `.tools\downloads\vs_BuildTools.exe`로 받고 `Install-WindowsBuildTools.cmd`를 실행합니다(`.tools\visualstudio-buildtools`에 설치).
2. Flutter: `powershell -File tools\setup-flutter.ps1`(Flutter 3.47.7을 `.tools\flutter`에 설치).
3. Windows 클라이언트 설치 파일을 만들려면 NSIS 3.13 zip을 `.tools\nsis-download\nsis-3.13.zip`에 두고 같은 폴더에 풀어 둡니다. 7-Zip의 `7z.exe`도 `.tools\7zip\`에 둡니다. `tools/build-windows-installer.ps1`이 NSIS 해시를 확인하고, 만든 설치 파일을 7-Zip으로 다시 풀어 검사합니다.

### Windows 클라이언트

```powershell
Build-Client.cmd
powershell -File tools\build-windows-installer.ps1 -Product client -Version <x.y.z> -Payload apps\windows_client\build\windows\x64\runner\Release
```

### Android 클라이언트

JDK 21과 Android SDK 36을 `tools/android-workspace.ps1`이 가리키는 `.tools` 경로에 두고 `powershell -File tools\build-android.ps1 -Version <build.gradle.kts의 versionName>`을 실행합니다. 처음에는 `-CreatePreviewKey`를 붙이면 자신의 서명 키가 `.local\android`에 만들어집니다. Gradle 래퍼(`apps/windows_client/android/gradlew`, Flutter가 만드는 표준 래퍼)는 저장소에 들어 있습니다.

### Host

Host 설치 파일은 EXE 안에 Windows Installer 패키지가 들어 있고(`apps/windows_host/installer/host_installer.cpp`), `tools\host-installer\`의 스크립트로 만듭니다. x264 고정 커밋과 NASM 준비부터 합니다([apps/windows_host/third_party/x264/README.md](../apps/windows_host/third_party/x264/README.md)). 이 스크립트들은 `.tools`에 준비된 도구를 읽습니다. 경로는 각 스크립트 맨 위에 있습니다(MSVC 환경 스크립트 `.tools\msvc-portable\env.ps1`, CMake 3.31.8, Ninja, libvirtualdisplay SDK, x264, NASM, Opus 1.6.1 tarball). 이 가운데 컴파일러 부분을 준비해 주는 스크립트는 없습니다.

1. `powershell -File tools\fetch-libvirtualdisplay.ps1`로 libvirtualdisplay SDK 소스와 서명된 드라이버를 받습니다.
2. `powershell -File tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native`가 `MultitapHost.exe`와 `MultitapHostTool.exe`를 빌드하고 C++ 테스트를 실행합니다.
3. `powershell -File tools\build-portable-windows.ps1 -Variant HostSettings`가 Host 설정(Host 설정 모드의 클라이언트)을 빌드합니다.
4. `tools\host-installer\reversion-msi.ps1`이 이전 릴리스의 패키지와, 설치된 Host와 같은 구조의 payload 폴더로 새 버전의 패키지를 만들고, `tools\host-installer\build.ps1 -Msi <패키지> -Version <x.y.z>`가 그것을 `Multitap-Host-<x.y.z>-windows-x64.exe`로 묶습니다. 패키지는 항상 바로 앞 패키지에서 만들고 그 첫 패키지는 이 저장소에 없으므로, 새로 받은 저장소만으로는 이 단계를 끝낼 수 없습니다.

payload의 드라이버 파일은 고정된 다운로드에서 오고, 모두 해시와 서명을 확인합니다: `tools\build-virtual-display.ps1`(MttVDD와 helper, `bin\driver`), `tools\fetch-libvirtualdisplay.ps1`(`bin\native-display-driver`), `tools\fetch-usb-drivers.ps1`(usbip-win2, `bin\usb-driver`), `tools\fetch-winfsp.ps1`(WinFsp, `bin\winfsp`).

### 테스트

구성요소별 테스트 실행 방법은 [CONTRIBUTING.md](../CONTRIBUTING.md#테스트)에 있습니다.

### 서명 키

공식 릴리스는 이 프로젝트의 Ed25519 키로 서명하고, 클라이언트와 Host는 소스에 고정된 공개키(`native/windows_admin/release_signature.hpp`)로 확인합니다. 직접 빌드한 판을 배포하려면 자신의 키를 만들고(`tools/windows-release-signature.mjs`, 서버 업데이트용 `tools/create-server-update-key.mjs`) 고정 공개키를 바꿔야 합니다. 개인키는 저장소에 넣지 마세요. 포크에서 바꿀 곳은 [releasing.md](releasing.md#once)에 있습니다(영어).
