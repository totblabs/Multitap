# 서버 접속 주소 관리

`server-access-v1`은 한 설치의 클라이언트용 접속 주소(HTTPS, 선택적으로 HTTP)와 서버 내부 수신 포트, TLS 방식, 믿을 프록시를 관리한다. 관리자 콘솔의 **설정 → 서버 접속 설정** 화면이다. 외부 origin의 포트와 서버 프로세스의 수신 포트는 서로 다른 값이다. 한 브로커 프로세스가 모든 origin에서 같은 계정·할당·중계 상태를 쓴다.

변경을 적용하는 주체는 설치 방식에 따라 둘이다.

- 업데이트 도구가 있는 설치(`server/update/agent.mjs`, 브로커 쪽 `MULTITAP_UPDATE_SOCKET`): 콘솔의 검사·저장을 업데이트 도구가 받아 적용한다. 아래 "검사와 적용"이 이 방식이다.
- `compose.yaml`로 띄운 Docker Compose 설치(업데이트 도구 없음): 브로커가 정책을 데이터 볼륨에 스스로 보관하고 적용한다. 아래 "Docker Compose 설치: 스스로 관리"가 이 방식이다. 콘솔 화면과 API는 같다.

그 밖의 설치(업데이트 도구도 없고 Docker도 아님)에서는 화면이 현재 값만 보여 주고 "이 설치에서는 접속 설정을 바꿀 수 없어요"라고 안내한다.

## 설정 규칙

- 기본 권장은 HTTPS다. HTTPS는 리버스 프록시의 TLS 종료나 서버의 직접 TLS 중 하나를 고른다. 직접 TLS는 인증서와 개인 키의 일치, 유효 기간, 이름, 서비스 계정의 읽기 권한을 검사하고 신뢰 검증을 건너뛰지 않는다.
- HTTP는 운영자가 명시적으로 켠 경우에만 일반 접속에 쓴다(암호화 없음으로 표시).
- HTTPS와 HTTP의 허용 origin은 각각 정확히 등록하고, 그중 하나를 대표 주소로 고른다. 포워딩 헤더는 등록한 정확한 프록시 IP에서 온 요청만 믿는다.
- 데이터 폴더의 보호된 `server-access.json`이 있으면 환경 변수보다 우선한다. 저장된 revision은 정규화한 설정의 SHA-256과 같아야 한다. 관리되던 이 파일이 사라져도 환경 변수 방식으로 돌아가지 않고 시작을 거부한다.
- 환경 변수만 쓰는 설치에서 프록시 IP를 정하지 않으면 루프백 주소만 믿는다. 다른 서버의 프록시를 쓰면 그 IP를 `MULTITAP_TRUSTED_PROXIES`에 넣어야 한다.
- 운영 모드에서 저장된 설정도 공개 주소도 없으면 초기 관리자 설정만 가능한 bootstrap 상태로 시작한다. 이 상태를 일반 HTTP 허용이나 HTTPS 준비 완료로 표시하지 않는다. 초기 설정의 로컬 주소는 요청을 실제로 받은 소켓의 주소·포트와 대조한다.
- 저장된 접속 정책이 생긴 뒤에는 관리자 로그인 쿠키를 origin별로 나눈다(HTTP·HTTPS, 포트가 다르면 각각 로그인).
- 보호 파일은 검사한 같은 파일 핸들에서 크기를 제한해 읽는다. 현재 설정과 마지막 기록이 다르거나 읽기에 실패하면 적용 완료 대신 복구 필요로 표시하고 변경을 막는다.

## 검사와 적용

아래 적용 순서는 업데이트 도구가 있는 설치의 것이다. 관리 API는 두 방식이 같다.

관리 API(`GET /api/v1/admin/server-access`, `POST .../server-access/test`, `POST .../server-access/save`)는 적용된 설정과 대기 중인 설정을 구분한다. 검사는 설정과 기준 revision에 묶인 `testId`를 발급하며 5분 동안 유효하다. 설정을 고치거나 기준 revision이 바뀌면 다시 검사해야 한다.

검사 통과는 적용 전 사전 검사다. 아직 열리지 않은 외부 주소의 실제 TLS·라우팅까지 확인했다는 뜻이 아니다. 저장은 명시적 확인(`confirm:true`)이 필요하고 다음 순서로 진행한다.

1. 중계 세션, 클립보드·드라이브·USB·인쇄 채널, 화면·세션 정리·Host 업데이트·전원 작업이 모두 없고 마지막 활동 뒤 60초가 지날 때까지 최대 5분 기다린다. 연결된 세션을 강제로 끊지 않는다.
2. 브로커는 새 작업을 받지 않도록 막고(업데이트 도구만 아는 일회용 값으로 확인), 쓰기 직전에 다시 확인한다.
3. 현재 설정, 환경 파일, 업데이터 설정을 보관하고 설치가 소유한 파일과 서비스만 바꾼다.
4. 활성화된 모든 외부 origin에서 실제 `/healthz`를 확인한다. 서비스, 버전, 설치 ID, 정확한 origin과 revision, 허용 주소가 모두 맞아야 성공이다. 리디렉션이나 인증서 예외로 대신하지 않는다.
5. 실패하면 이전 설정을 복원하고 그 상태를 다시 확인한다. 확인하지 못한 복구는 완료로 표시하지 않고 막힌 상태를 유지한다.

저장 응답이 유실되면 같은 operation ID의 상태를 조회한다. 저장 요청을 자동으로 다시 보내지 않는다. 재시작 뒤 쓰기 전에 멈춘 작업은 실제 현재 revision과 설치 상태를 확인한 경우에만 "변경 없음"으로 정리한다.

## Docker Compose 설치: 스스로 관리

`MULTITAP_DEPLOYMENT_KIND=docker`이고 `MULTITAP_UPDATE_SOCKET`, `MULTITAP_SERVER_ACCESS_FILE`이 없는 설치다(`server/src/self-access.mjs`, `config.mjs`). 주소, 대표 주소, 믿을 프록시, 공인 IP 따라가기를 콘솔의 서버 접속 설정에서 정하고, 서버의 `multitap address`·`multitap public-ip`는 같은 정책을 읽고 쓰는 복구 수단이다. 정책이 있는 곳은 하나뿐이다.

### 보관

데이터 볼륨의 `access/` 폴더, 브로커 자신의 계정(UID 1000) 소유.

| 파일 | 내용 |
| --- | --- |
| `policy.json` | 저장된 정책(`server-access.json`과 같은 모양). 공인 IP를 따라가는 동안에는 `publicIp {follow, origin}`이 더 있다 |
| `identity` | 설치 ID. 첫 정책과 함께 만든다 |
| `journal.json` | 진행 중인 저장과 그 결과 |
| `policy.before-cli.json` | `access-cli.mjs revert`가 되돌릴 이전 정책 |

브로커는 자기 계정 소유이고 다른 사용자가 쓸 수 없는 파일만 읽는다. root로 만든 파일은 받지 않는다.

### 컨테이너가 정하는 것

수신 주소·포트, TLS 방식, 인증서 경로, Docker가 게시하는 포트는 Compose와 `.env`가 정한다. 정책은 저장할 때와 읽을 때 항상 이 값에 맞춰진다. 화면은 이 값을 고정으로 보여 준다(상태의 `fixed`).

- 서버 IP(`MULTITAP_SERVER_ADDRESS`)로 만든 HTTP 주소는 게시 포트(`MULTITAP_PUBLISH_PORT`)를 써야 한다(`server_access_published_port_conflict`).
- 포트가 루프백에만 게시돼 있으면 네트워크용 HTTP 주소를 받지 않는다(`server_access_published_loopback_conflict`).
- 직접 TLS(`compose.direct-https.yaml`)에서는 HTTP 주소를 함께 쓸 수 없다(`server_access_direct_tls_http_conflict`).

게시 포트를 바꾸는 일은 서버에서만 할 수 있다. `multitap address`가 `.env`의 `MULTITAP_PUBLISH_PORT`를 주소의 포트에 맞춘다.

### `.env`의 주소 줄

`MULTITAP_PUBLIC_ORIGIN`, `MULTITAP_HTTP_ORIGIN`, `MULTITAP_ALLOW_INSECURE_HTTP`, `MULTITAP_TRUSTED_PROXIES`는 정책을 저장한 적 없는 서버의 시작값이다. 정책이 저장되면 읽지 않는다. 저장된 정책과 다른 주소가 남아 있으면 상태의 `envAddressesIgnored`가 참이고 시작할 때 `env_addresses_ignored` 경고가 남는다.

`multitap`은 정책이 저장된 뒤 컨테이너를 어차피 새로 만드는 두 시점(`multitap address <주소>`, `multitap update`)에 이 줄들을 비운다. 남겨 두면 정책이 사라졌을 때(초기화 등) 이미 닫은 주소가 되살아나기 때문이다. 줄이 비어 있고 정책도 없는 서버는 서버 IP의 처음 설정 주소만 연다.

### 저장과 적용

콘솔의 검사·저장 API는 위와 같다. 검사는 설정만 확인하고, 실제 주소는 적용 뒤에 확인한다.

1. 끝나지 않은 서버 작업(Host 업데이트, 화면 설정, 전원 작업)이 끝나기를 최대 5분 기다린다. 이 단계는 취소할 수 있다.
2. 남은 연결을 서버가 끊고, 저장 내용을 `journal.json`에 먼저 적은 뒤 `policy.json`을 바꾼다.
3. 브로커가 종료 코드 75로 끝나고, 컨테이너의 entrypoint(`server/docker-entrypoint.sh`)가 같은 컨테이너에서 브로커를 다시 시작한다. 컨테이너는 다시 만들어지지 않는다.
4. 새 프로세스가 HTTPS와 HTTP 각각의 기본 주소로 자기 자신의 `/healthz`를 확인한다(설치 ID, revision, origin). 추가 주소는 확인하지 않는다.
5. 닿지 못하거나 새 정책으로 시작하지 못하면 이전 정책을 되돌리고 다시 시작한다. 결과는 `failed_rolled_back`으로 보인다.

공인 IP 따라가기만 켜고 끄는 저장은 주소가 바뀌지 않으므로 다시 시작하지 않는다.

### 공인 IP 따라가기

켜져 있는 동안만 브로커가 10분마다 `https://1.1.1.1/cdn-cgi/trace`로 공인 IPv4를 확인하고, `http://<그 IP>:<게시 포트>`를 마지막 추가 HTTP 주소로 둔다. IP가 바뀌면 같은 저장 절차로 그 주소를 바꾼다. 다른 HTTP 주소가 하나 있어야 하고, 따라가는 주소는 대표 주소가 될 수 없다(`server_access_public_ip_conflict`). HTTP 주소가 가득 차 있으면 등록하지 않고 상태에 `server_access_http_addresses_full`을 남긴다.

### 명령줄

`server/src/access-cli.mjs`를 서비스의 일회용 컨테이너에서 실행한다. 서버가 켜져 있어도, 꺼져 있어도 된다. 서비스 계정으로 실행한다(`--user 0` 금지).

```sh
docker compose run --rm --no-deps -T --entrypoint node broker /app/src/access-cli.mjs show
```

| 명령 | 하는 일 |
| --- | --- |
| `show` | 쓰는 주소와 컨테이너가 정한 값 |
| `set <주소>... [--primary <주소>] [--trusted-proxies <IP,IP\|none>] [--follow on\|off] [--force]` | 주소 전체를 바꾼다. 외부 확인 없이 쓰므로 잠긴 서버의 복구 수단이다. 다음 시작 때 읽는다 |
| `public-ip status \| on [--address <IPv4>] \| off` | 따라가기 스위치. 켜고 끄는 것은 실행 중인 서버가 다음 확인(10분 안)에 읽는다 |
| `revert` | 마지막 `set`·`public-ip`가 바꾸기 전의 정책으로 |
| `reset` | 저장된 정책, 진행 기록, 되돌리기 사본을 지운다. `.env` 줄이 다시 시작값이 된다 |

출력은 `key=value` 줄이고 첫 줄이 `ok=1` 또는 `ok=0`과 `code=<이유>`다. 종료 코드는 0(완료), 2(거절), 64(사용법). 콘솔의 저장이 진행 중이면 `server_access_busy`로 거절하고, `--force`는 서버를 멈춘 상태에서만 쓴다.

`multitap address`와 `multitap status`는 `show`를 보여 주고, `multitap address <주소>...`는 `.env`의 게시 포트를 맞춘 뒤 `set`을 실행하고 컨테이너를 새로 만든다. 새 주소로 시작하지 못하면 `revert`와 이전 `.env`로 되돌린다. `multitap public-ip on|off`는 `public-ip`를 실행한다.

### 복원과 백업

`multitap restore`는 어떤 백업을 되돌려도 `access/`를 이 서버의 것 그대로 둔다. 다른 서버의 복사본에는 그 서버의 주소와 설치 ID가 들어 있고, 같은 서버의 예전 복사본에는 그때의 주소가 들어 있기 때문이다. 복원이 실패해 안전 백업으로 돌아갈 때만 볼륨 전체(`access/` 포함)를 되돌린다.

### 상태 응답에 더 있는 것

`GET /api/v1/admin/server-access`의 응답에는 이 방식에서 `selfManaged: true`, `fixed {listenHost, listenPort, httpsMode, certFile, keyFile, idlePorts, publishedPort, publishedHost, serverAddress}`, `publicIp {offered, follow, origin, errorCode?}`, `envAddressesIgnored`, `dockerPorts`, `gateway`(컨테이너의 기본 게이트웨이. 화면의 "프록시가 이 서버에 있어요"가 믿을 프록시에 넣는 주소)가 있다. 모든 설치의 응답에 `udpMedia {running, port, derivedPort, media, relays, hostRoute}`가 있다.

UDP 미디어는 이 방식에서 `.env`의 중계 목록 없이 동작한다. 앱과 Host 각각에 자기가 접속한 주소의 호스트와 게시된 UDP 포트(`MULTITAP_UDP_PUBLISHED_PORT`, Compose가 넘김)를 준다. UDP 포트가 루프백에만 게시됐거나, 접속 주소가 IPv6 숫자 주소이거나, 목록이 하나만 설정된 경우에는 쓰지 않는다([Docker 배포](docker-deployment.md#udp-미디어)).

`POST /api/v1/admin/server-restart {confirm:true}`는 이 방식에서 서버를 다시 시작한다(202). 저장이나 복원이 진행 중이면 409, 다른 설치에서는 503 `server_restart_unavailable`이다.

## HTTP 설치의 서버 업데이트와 HSTS

HTTP만 쓰는 설치의 서버 업데이트는 root가 보호하는 업데이터 설정에 `allowInsecureHttp`가 명시돼 있어야 한다(기본 거부). HTTP를 켜도 Ed25519 서명, 정확한 크기·SHA-256, 고정 상대 다운로드 경로, 리디렉션 거부는 그대로다. Proxmox 인증서 정책과는 무관하다.

이 설치의 이전 허용 origin에 있던 업데이트 목록(`/downloads/server-updates.json`)만 새 대표 주소를 따라간다. 운영자가 따로 지정한 외부 업데이트 서버와 공개 키는 그대로 둔다. 바뀐 업데이터 설정도 다른 접속 설정과 함께 백업·복구한다.

같은 호스트 이름으로 HTTP를 명시적으로 켜면 HTTPS 응답이 `Strict-Transport-Security: max-age=0`을 보낸다. 브라우저에 이전 HSTS가 남아 있으면 먼저 HTTPS 관리 화면을 한 번 열어 해제해야 한다. 외부 프록시가 HSTS를 따로 강제하면 그 설정도 확인한다. 다른 호스트 이름의 HTTP나 HTTPS 전용 구성의 HSTS는 바꾸지 않는다.

## 업데이트 도구가 관리하는 Docker 설치

업데이트 도구가 관리하는 Docker 설치에서는 현재 포트 매핑(`dockerPorts`)과 적용 예정 매핑(`plannedDockerPorts`)을 구분해 보여 준다. 각 항목은 실제이거나 명시적으로 계획된 `{hostPort, containerPort, protocol:'tcp', scheme}`이며, 외부 origin의 포트에서 호스트 포트를 추정하지 않는다. 새 매핑은 정한 내부 수신 포트를 기준으로 계획하고, 적용 전에는 NAT 목적지가 준비됐다고 표시하지 않는다. 공유기·프록시의 외부 포트와 Docker 호스트 포트는 따로 확인해야 한다.

설정은 Compose 프로젝트 폴더의 `multitap-server-access.json`과 덮어쓰기 파일 `multitap-server-access.yaml`에 쓴다. 포트 교체에 YAML `!override`를 쓰므로 Docker Compose 2.24.4 이상이 필요하다. 다른 기능의 포트는 보존한다. 직접 TLS는 이미 관리 중인 읽기 전용 인증서 폴더를 쓰며, 설정 변경을 이유로 임의의 호스트 폴더, Docker 소켓, 새 권한이나 장치를 컨테이너에 붙이지 않는다. 적용과 복구는 확인된 같은 프로젝트·볼륨·이미지만 대상으로 한다.

## systemd 업데이터 unit

Linux 직접 설치의 업데이터 unit은 `ProtectSystem=strict`로 실행되므로 `/etc/multitap`에 쓰려면 `ReadWritePaths`에 그 경로가 있어야 한다. 새 provisioner와 새 업데이트 agent는 시작할 때 프로젝트가 소유한 정확한 unit(root 소유, 고정 실행 경로)인지 확인하고, 원문과 모드를 보관한 뒤 기존 `ReadWritePaths`에 `/etc/multitap`만 더하고 `daemon-reload`를 한다. 다른 설정과 원문은 보존한다. 진행 중인 업데이트 도중에는 업데이터를 재시작하지 않는다. 이미 실행 중인 agent가 옛 unit에서 떴다면 소켓이나 새 작업을 열기 전에 한 번 정상 종료하고, 기존 `Restart=always`가 새 설정으로 다시 띄운다. 중간에 멈추면 재시작 때 다시 확인하고, 실패하면 정확한 이전 원문을 복원한다. 그 사이 관리자가 바꾼 파일은 덮어쓰지 않고 복구 필요 진단을 남긴다.
