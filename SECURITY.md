# Security policy

[한국어](#보안-문제-알리기)

## Supported versions

Security fixes go into the latest released broker, Host, Windows client and Android client only. Please first check that the problem still happens after updating.

## How to report

**Do not open a public issue for a vulnerability.**

Report it through this repository's **Security → Report a vulnerability** (GitHub private vulnerability reporting). Only the repository maintainers can see the report. The project has no other reporting channel (no e-mail address).

These details help us check it quickly:

- The affected part and version (broker, Host, client, installer)
- Steps to reproduce and the access needed (signed in or not, administrator or not, same network or not)
- The expected impact (authentication bypass, access to another user's VM, remote code execution, information disclosure and so on)

Do not include real passwords, tokens, Proxmox API tokens, server addresses or other secrets. Redact them if needed.

## Handling

This is a personal project without a guaranteed response time. Reports are checked and answered, and the details are published after a fixed release is out. Please keep the details private until then.

## Out of scope

- Problems in the operator's own server, reverse proxy, TLS certificates or Proxmox itself
- What a user who already has administrator rights can do with administrator features
- Problems that only reproduce on outdated versions

---

## 보안 문제 알리기

[English](#security-policy)

### 지원 버전

보안 수정은 가장 최근에 공개한 브로커, Host, Windows 클라이언트, Android 클라이언트에만 들어갑니다. 업데이트 후에도 재현되는지 먼저 확인해 주세요.

### 알리는 방법

보안 취약점은 **공개 이슈로 올리지 마세요.**

이 저장소의 **Security → Report a vulnerability**(GitHub 비공개 취약점 신고)로 알려 주세요. 신고 내용은 저장소 관리자에게만 보입니다. 이 프로젝트는 다른 신고 창구(메일 등)를 두지 않습니다.

다음 내용을 적어 주시면 빠르게 확인할 수 있습니다.

- 영향을 받는 구성요소와 버전(브로커, Host, 클라이언트, 설치 파일)
- 재현 순서와 필요한 권한(로그인 여부, 관리자 여부, 같은 네트워크 여부)
- 예상되는 영향(인증 우회, 다른 사용자의 VM 접근, 원격 코드 실행, 정보 노출 등)

신고에 실제 비밀번호, 토큰, Proxmox API 토큰, 서버 주소 같은 비밀값을 넣지 마세요. 필요하면 가린 값으로 보내 주세요.

### 처리

개인 프로젝트라 정해진 응답 시간은 없습니다. 받은 신고는 확인 후 답하고, 수정판을 낸 다음에 내용을 공개합니다. 수정 전에는 세부 내용을 공개하지 말아 주세요.

### 범위 밖

- 운영자가 직접 설정한 서버, 리버스 프록시, TLS 인증서, Proxmox 자체의 문제
- 관리자 권한을 이미 가진 사용자가 관리자 기능으로 할 수 있는 일
- 오래된 버전에서만 재현되는 문제
