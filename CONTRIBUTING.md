# Contributing

[한국어](#기여하기)

## License

Multitap is AGPL-3.0-only, and contributions are accepted under the same license (inbound = outbound). There is no CLA and no copyright assignment. Only send code you wrote yourself, or code under a license that can be distributed with AGPL-3.0-only. If you bring in third-party code, name its source and license in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## This repository

This repository is exported from the development repository at every release. Issues and pull requests are welcome here, and accepted changes appear here with the next release. Report security problems privately as described in [SECURITY.md](SECURITY.md), not in an issue.

## Tests

| Part | How to run |
|---|---|
| Broker | `npm ci` and `npm test` in `server/` (Node.js 24) |
| Proxmox setup script | `python3 server/test/proxmox_setup_test.py` (and the other `server/test/proxmox_*_test.py`; the TLS tests need `openssl` on `PATH`) |
| Docker install scripts | `bash -n deploy/docker/install.sh` and `bash -n deploy/docker/multitap` |
| Clients (Windows and Android UI, Dart) | After `tools\setup-flutter.ps1`, run `..\..\flutter.cmd test` in `apps\windows_client` (Windows) |
| Host | `tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native` builds the Host and runs its C++ tests (setup in the [README](README.md#build-from-source)) |
| noVNC vendoring | `python tools\test-vendor-novnc.py` |
| Native QA tools | `tools\*-qa\` (each folder has a README or a run script that says how to run it, or is built by the Host's CMake project) |

Add or update tests for what you change. A passing build or test is not a claim that something works on a real VM: say in the pull request what you verified.

## Please

- Never put passwords, tokens, keys, real server addresses or personal paths into code, tests or docs. For examples use `vdi.example.com`, `192.0.2.x` and `C:\Users\<user>`.
- Keep changes small, with one purpose per pull request.
- User-facing text is short and ships in Korean and English. Korean is the source; the English says the same thing at about the same length. Keep commands, paths, codes and placeholders the same in both.

---

## 기여하기

[English](#contributing)

### 라이선스

Multitap는 AGPL-3.0-only입니다. 기여한 코드와 문서도 같은 라이선스(AGPL-3.0-only)로 들어갑니다(inbound = outbound). CLA나 저작권 양도는 없습니다. 직접 작성했거나 AGPL-3.0-only와 함께 배포할 수 있는 라이선스의 코드만 보내 주세요. 외부 코드를 가져오면 출처와 라이선스를 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)에 적어 주세요.

### 이 저장소

이 저장소는 릴리스마다 개발 저장소에서 내보낸 사본입니다. 이슈와 풀 리퀘스트는 여기에서 받고, 받아들인 변경은 다음 릴리스와 함께 이곳에 들어갑니다. 보안 문제는 이슈 대신 [SECURITY.md](SECURITY.md)의 방법으로 알려 주세요.

### 테스트

| 구성요소 | 실행 |
|---|---|
| 브로커 | `server/`에서 `npm ci`, `npm test` (Node.js 24) |
| Proxmox 설정 스크립트 | `python3 server/test/proxmox_setup_test.py`(와 다른 `server/test/proxmox_*_test.py`. TLS 테스트는 `PATH`에 `openssl`이 필요) |
| Docker 설치 스크립트 | `bash -n deploy/docker/install.sh`, `bash -n deploy/docker/multitap` |
| 클라이언트(Windows·Android 화면, Dart) | `tools\setup-flutter.ps1` 후 `apps\windows_client`에서 `..\..\flutter.cmd test` (Windows) |
| Host | `tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native`가 Host를 빌드하고 C++ 테스트를 실행합니다(준비는 [README](README.ko.md#소스에서-빌드)) |
| noVNC 반입 | `python tools\test-vendor-novnc.py` |
| 네이티브 QA 도구 | `tools\*-qa\`(폴더마다 실행 방법을 적은 README나 실행 스크립트가 있거나, Host의 CMake 프로젝트가 빌드합니다) |

바꾼 부분의 테스트를 함께 추가하거나 고쳐 주세요. 빌드나 테스트 통과만으로 실제 VM에서 동작한다고 쓰지 않습니다. 확인한 범위를 풀 리퀘스트에 적어 주세요.

### 지켜 주세요

- 비밀번호, 토큰, 키, 실제 서버 주소, 개인 경로를 코드·테스트·문서에 넣지 않습니다. 예시는 `vdi.example.com`, `192.0.2.x`, `C:\Users\<user>`를 씁니다.
- 변경은 작게 나눕니다. 한 풀 리퀘스트에는 한 가지 목적만 담습니다.
- 화면 문구는 짧게, 한국어와 영어로 씁니다. 한국어가 원문이고, 영어는 같은 뜻을 비슷한 길이로 씁니다. 명령·경로·코드·자리표시자는 두 언어에서 같게 둡니다.
