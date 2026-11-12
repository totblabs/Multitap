# Windows VM 연결하기

Windows VM(또는 Windows PC)에 Host를 설치하고 승인하는 순서는 가이드북 4장에 있습니다.

- 한국어: [4. Host 설치 — VM과 로컬 PC](guide/04-Host-설치.md)
- English: [4. Install Host — VMs and local PCs](guide/en/04-host-install.md)

짧게 요약하면 다음과 같습니다.

1. 데스크톱이 될 Windows에서 `서버주소/get/host`로 설치 파일을 받아 실행합니다. Host 설정이 열리면 설치 마무리(가상 화면 드라이버와 WinFsp)를 끝냅니다.
2. Host 설정의 **서버 연결**에서 서버 주소를 넣고 **서버 연결 테스트**와 **연결 요청**을 누릅니다.
3. 관리자 페이지의 **데스크톱** 맨 위 **승인 대기**에서 확인 코드가 Host 화면과 같은지 보고 **확인하고 승인**을 누릅니다.
4. Host 설정의 **Windows 로그인**에 Windows 계정을 저장하고, **가상 화면**과 **장치**를 확인합니다.
5. 관리자 페이지의 **사용자**에서 데스크톱을 배정합니다([5장](guide/05-사용자와-VM-배정.md)).

브로커 설치와 Proxmox 연결은 [2장](guide/02-서버-설치.md)과 [3장](guide/03-Proxmox-연결.md), Docker로 직접 운영할 때는 [Docker 배포](docker-deployment.md)를 보세요.
