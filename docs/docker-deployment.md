# Docker 배포 (브로커)

Multitap 브로커(관리자 페이지, 인증 중계, 다운로드 서버)를 Docker Compose로 운영하는 방법입니다. 이미지에는 Windows 클라이언트·Host 설치 EXE와 Android APK(3개 ABI)가 함께 들어 있습니다. 첫 시작 때 이 파일들이 데이터 볼륨으로 복사되고, 클라이언트·Host·휴대폰은 이 서버의 `/get/client`, `/get/host`, `/get/android`와 업데이트 목록에서 받습니다.

배포자의 도메인을 기본값으로 쓰지 않습니다. 각 설치는 자신의 주소를 씁니다: 도메인과 인증서가 있으면 HTTPS, 같은 네트워크에서만 쓰면 서버 IP로 HTTP, 또는 둘 다. 자체 업데이트 도구가 있는 Linux 직접 설치(systemd)는 이 문서와 별개이며 그대로 동작합니다.

## 준비물

- Linux 서버, Docker Engine, Docker Compose 플러그인(`docker compose`).
- HTTPS를 쓸 때만: 공개 도메인과 TLS 인증서. 보통 리버스 프록시(nginx, Caddy 등)에서 TLS를 끝냅니다.
- 포트: 브로커 TCP 18480, UDP 미디어 18481(방화벽과 공유기에서 열기).

| 입력 위치 | HTTPS 예시 | HTTP 예시 (같은 네트워크) |
| --- | --- | --- |
| `.env` | `MULTITAP_PUBLIC_ORIGIN=https://vdi.example.com` | `MULTITAP_HTTP_ORIGIN=http://192.168.0.10:18480` |
| Windows 앱·Android 앱의 서버 주소 | `https://vdi.example.com` | `HTTP`를 고르고 `192.168.0.10:18480` |
| 관리자 페이지 | `https://vdi.example.com` | `http://192.168.0.10:18480` |
| 리버스 프록시 업스트림 | `http://127.0.0.1:18480` (같은 서버) | 없음 |
| Proxmox 연결 설정의 Proxmox 주소 | 실제 Proxmox 관리 주소, 예: `https://pve.example.com:8006` | 같음 |

## 처음 시작

1. `compose.yaml`과 `.env.example`이 있는 폴더에서 설정 파일을 만들고, `.env`에 서버의 IP를 넣습니다.

   ```sh
   cp .env.example .env
   sed -i 's/^MULTITAP_SERVER_ADDRESS=.*/MULTITAP_SERVER_ADDRESS=192.168.0.10/' .env
   ```

2. 이미지를 받고 시작합니다.

   ```sh
   docker compose pull && docker compose up -d
   ```

3. 브라우저로 `http://192.168.0.10:18480` 을 엽니다. 첫 설정 화면이 바로 나옵니다.

   `admin` / `admin`으로 로그인하고, 화면이 묻는 1회용 설정 코드를 넣습니다. 코드와 열 주소는 서버 로그에 함께 나옵니다.

   ```sh
   docker compose logs broker | grep initial_setup
   ```

   컨테이너가 다시 시작될 때마다 새 코드가 나옵니다. 새 아이디와 비밀번호를 정하면 Proxmox 연결 화면으로 갑니다. 이 주소는 접속 주소를 정하기 전까지 처음 설정에만 쓰이고, 앱·Host 연결과 다운로드는 거절됩니다.

4. 접속 주소를 정합니다. 관리자 페이지의 **서버 접속 설정**에 HTTPS 주소나 HTTP 주소를 넣으면 `.env`에 넣을 줄이 나옵니다. `.env`의 같은 이름 줄을 바꾸고 다시 시작합니다.

   ```sh
   docker compose up -d
   ```

   - HTTPS만: `MULTITAP_PUBLIC_ORIGIN=https://vdi.example.com`. 프록시가 같은 서버에 있으면 `MULTITAP_PUBLISH_PORT=127.0.0.1:18480`으로 바깥에서 직접 오는 연결을 막고, 아래 5번을 합니다.
   - HTTP만: [HTTP로 쓰기](#http로-쓰기-같은-네트워크-인증서-없이)의 세 줄.
   - 둘 다: 두 주소를 모두 넣습니다. 같은 계정과 데스크톱을 씁니다.

   Compose 설치에는 서버 접속 설정을 직접 적용할 root 업데이트 도구가 없어서 `.env`로 적용합니다.

5. (HTTPS 프록시) 프록시를 신뢰하게 합니다. 같은 서버의 프록시가 보낸 요청은 컨테이너에서 `127.0.0.1`이 아니라 Docker 네트워크 게이트웨이 주소로 보입니다. 이 주소를 넣지 않으면 프록시를 거친 요청이 `403 http_disabled`로 거절됩니다.

   ```sh
   docker network inspect multitap_default -f '{{range .IPAM.Config}}{{.Gateway}}{{end}}'
   # 예: 172.18.0.1
   echo 'MULTITAP_TRUSTED_PROXIES=172.18.0.1' >> .env
   docker compose up -d
   ```

   프록시가 다른 서버에 있으면 그 서버의 주소를 넣습니다. `docker compose down`으로 네트워크를 지웠다면 게이트웨이가 바뀔 수 있으니 다시 확인합니다.

6. 상태를 확인합니다.

   ```sh
   docker compose ps
   curl -fsS https://vdi.example.com/healthz      # 또는 http://192.168.0.10:18480/healthz
   ```

처음부터 주소를 알면 1번에서 `MULTITAP_PUBLIC_ORIGIN`이나 `MULTITAP_HTTP_ORIGIN`을 바로 넣어도 됩니다. 이때도 브라우저로 그 주소를 열면 첫 설정 화면이 나옵니다.

관리자 페이지 모니터링의 날짜는 `.env`의 `TZ`(IANA 이름, 예: `Asia/Seoul`)를 따르고, 비워 두면 UTC입니다. `deploy/docker/install.sh`로 설치하면 `.env`를 새로 만들 때 서버의 시간대를 넣습니다.

## HTTP로 쓰기 (같은 네트워크, 인증서 없이)

> **HTTP는 암호화하지 않습니다.** 로그인 비밀번호, 원격 화면, 키 입력이 그대로 네트워크에 흐릅니다. 믿을 수 있는 내부망에서만 쓰고, 공유기 포트 전달로 인터넷에 열지 마세요.

`.env`:

```sh
MULTITAP_HTTP_ORIGIN=http://192.168.0.10:18480
MULTITAP_ALLOW_INSECURE_HTTP=1
MULTITAP_PUBLISH_PORT=0.0.0.0:18480
```

- HTTPS 주소(`MULTITAP_PUBLIC_ORIGIN`)와 함께 둘 수 있습니다. 같은 컨테이너 포트가 프록시의 HTTPS와 내부망의 HTTP를 함께 받습니다. `compose.direct-https.yaml`(브로커 자체 TLS)에서는 함께 쓸 수 없습니다.
- 주소의 포트는 게시 포트와 같아야 합니다. 게시는 `0.0.0.0:18480`(모든 IPv4 주소)으로 합니다. `18480`만 쓰면 IPv6 연결이 Docker를 거쳐 게이트웨이 주소로 들어와, 신뢰한 프록시처럼 보일 수 있습니다. 서버 방화벽이 있으면 TCP 18480을 엽니다.
- 주소를 쉼표로 이어 4개까지 넣을 수 있습니다. 예: `MULTITAP_HTTP_ORIGIN=http://192.168.0.10:18480,http://<공인 IP>:18480`(공유기가 TCP 18480을 이 서버로 전달할 때). 적은 주소로만 접속되고, 다른 주소는 `origin_not_allowed`로 막힙니다. 게시 포트는 첫 주소의 포트입니다.
- `MULTITAP_ALLOW_INSECURE_HTTP=1`이 없으면 시작하지 않습니다(`http_origin_requires_allow_insecure_http`).
- Windows 앱: 서버 설정 → `HTTP` → 주소 `192.168.0.10:18480` → 연결 테스트 → 확인 창의 **이 주소에 HTTP 사용** → 저장. 동의는 그 주소·포트에만 적용됩니다.
- 서버 로그에 `cleartext_http_enabled` 경고가 시작할 때마다 남습니다.

### 리버스 프록시 예 (nginx)

```nginx
map $http_upgrade $connection_upgrade { default upgrade; '' close; }
server {
  listen 443 ssl;
  server_name vdi.example.com;
  ssl_certificate     /etc/ssl/vdi.example.com/fullchain.pem;
  ssl_certificate_key /etc/ssl/vdi.example.com/privkey.pem;
  client_max_body_size 64m;
  location / {
    proxy_pass http://127.0.0.1:18480;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-Proto https;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $connection_upgrade;
    proxy_buffering off;
    proxy_read_timeout 1h;
  }
}
```

원격 화면과 Host 연결은 WebSocket을 쓰므로 `Upgrade`/`Connection` 전달과 긴 읽기 시간이 필요합니다.

프록시 없이 브로커가 직접 TLS를 쓰려면 `.env`에 `COMPOSE_FILE=compose.direct-https.yaml`, `MULTITAP_VERSION`, `MULTITAP_TLS_DIRECTORY`(cert.pem, key.pem; `1000:1000`이 읽을 수 있고 다른 사용자가 쓸 수 없게)를 넣습니다. 인증서는 운영자가 준비합니다. 컨테이너 헬스체크가 공인 CA로 인증서를 확인하므로 자체 서명 인증서는 `unhealthy`로 표시됩니다.

## UDP 미디어

브로커는 직접 설치와 같은 UDP 18481에서 미디어 중계를 시작하고, Compose가 이 포트를 서버에 게시합니다. 영상이 손실에 덜 멈추도록 UDP 경로를 쓰려면:

1. 서버 방화벽에서 UDP 18481을 열고, 공유기에서 이 서버로 UDP 18481을 전달합니다(TCP 443과 별도).
2. `.env`에 Host와 클라이언트가 찾아올 주소를 넣고 `docker compose up -d`를 실행합니다.

   ```sh
   MULTITAP_UDP_HOST_RELAYS=vdi.example.com:18481
   MULTITAP_UDP_CLIENT_RELAYS=vdi.example.com:18481
   MULTITAP_UDP_MEDIA=on
   ```

두 목록 중 하나라도 비어 있거나 UDP가 막혀 있으면 연결은 WebSocket(HTTPS 또는 HTTP)으로 그대로 동작합니다. 바깥 포트를 바꾸려면 `MULTITAP_UDP_PUBLISH_PORT`를 바꾸고 목록에도 그 포트를 적습니다. Host가 같은 내부망에 있고 공유기가 자기 공인 주소로 되돌아오는 연결을 처리하지 못하면 `MULTITAP_HOST_ROUTE`에 브로커의 내부 IPv4 주소를 넣습니다.

Host·클라이언트·서버가 모두 `udp-direct-v1`을 지원하면 단일 화면의 자동 비트레이트 세션은 인증된 UDP 직결도 시도합니다. 실패하면 UDP 중계, 이후 기존 WebSocket 경로를 사용합니다. 다중 화면과 고정 비트레이트 세션은 기존 경로를 유지합니다.

외부 주소 탐색은 기본 `stun.cloudflare.com:3478`을 사용합니다. 이 서버에는 공인 소켓 주소와 임의 트랜잭션만 전달되고 로그인 정보·화면은 전달되지 않습니다. `.env`에 `MULTITAP_STUN_SERVERS=off`를 넣으면 외부 STUN을 끌 수 있습니다. 자체 STUN을 쓰려면 `host:port`를 쉼표로 구분해 최대 2개 설정합니다. STUN을 꺼도 UDP 중계와 같은 LAN의 직결 시도는 유지됩니다. 클라이언트/Host 쪽 공유기 포트 개방이나 Windows 보안 설정 변경은 필요하지 않습니다.

## 업데이트

관리자 페이지 **업데이트** 화면이 실행 중인 버전과 새 버전 여부를 보여줍니다. 새 버전이 있으면 변경 내역과 실행할 명령을 그대로 보여줍니다. 관리자 페이지가 직접 이미지를 받거나 재시작하지는 않습니다.

```sh
sed -i '/^MULTITAP_VERSION=/d' .env && echo 'MULTITAP_VERSION=1.1.1' >> .env
docker compose pull && docker compose up -d
```

- 새 버전 정보는 프로젝트 GitHub Releases의 `server-updates-docker.json`에서 읽습니다. 피드 주소: `https://github.com/totblabs/Multitap/releases/latest/download/server-updates-docker.json`. 서버 업데이트용 Ed25519 키로 서명돼 있고, 브로커는 고정된 공개 키로 서명을 확인한 뒤에만 표시합니다. 서명이 맞지 않으면 "릴리스 서명이 맞지 않아 무시했어요"가 나옵니다.
- 관리자 페이지가 열려 있는 동안 최대 1시간에 한 번 확인하고, **업데이트 확인** 버튼은 바로 확인합니다. 바깥으로 나가는 확인을 끄려면 `MULTITAP_DOCKER_UPDATE_FEED=off`, 같은 서명 파일의 다른 https 주소를 쓰려면 그 주소를 넣습니다.
- 데이터 볼륨(`multitap-broker-data`)에 계정, 배정, `vault.key`, 다운로드가 있습니다. 이미지를 바꿔도 그대로 남습니다. `docker compose down -v`나 볼륨 삭제는 하지 않습니다.
- 새 이미지는 시작할 때 자신의 설치 파일 중 볼륨에 없는 것만 더합니다. 이전 설치 파일과 목록 항목은 지우거나 바꾸지 않습니다.
- 이미지는 GitHub Release마다 Actions가 linux/amd64로 만들어 `ghcr.io/totblabs/multitap-broker:<버전>`과 `:latest`로 올립니다. 릴리스 순서는 [docs/releasing.md](releasing.md)에 있습니다.
- `docker compose pull`은 레지스트리의 태그를 받습니다. 태그 자체는 서명 대상이 아닙니다.

## 소스에서 빌드

```sh
docker compose -f compose.yaml -f compose.build.yaml up -d --build
```

`.env`에 `COMPOSE_FILE=compose.yaml:compose.build.yaml`을 넣으면 `-f` 없이 같은 동작을 합니다. 이미지 버전은 `server/package.json`입니다.

빌드에는 `server/release-bundle/`에 검증된 다운로드 번들이 있어야 합니다. 없거나 맞지 않으면 빌드가 실패합니다.

- 다운로드 없는 개발용 이미지: 공개 저장소의 `server/release-bundle/`은 설치 파일이 없는 빈 번들입니다. `.env`에 `MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=true`를 넣고 빌드합니다. 이 설정이 없으면 빌드가 실패합니다. 기존 볼륨의 다운로드는 지워지지 않습니다.
- 공개 이미지와 같은 번들: 릴리스에 첨부된 번들 압축 파일(`multitap-release-bundle-<버전>.tar.gz`)을 `server/release-bundle/`에 풉니다.

  ```sh
  rm -rf server/release-bundle && mkdir server/release-bundle && tar -xzf multitap-release-bundle-<버전>.tar.gz -C server/release-bundle
  ```

- 빈 번들을 새로 만들 때(Node 없이):

  ```sh
  mv server/release-bundle server/release-bundle.previous
  mkdir -p /tmp/multitap-empty && echo '[]' > /tmp/multitap-empty/releases.json
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD/server:/w" -v /tmp/multitap-empty:/in:ro -w /w \
    node:24-bookworm-slim node src/download-bundle.mjs create /in/releases.json /in release-bundle --allow-empty
  ```

## 다운로드 번들 만들기 (배포 준비자)

검증된 파일을 한 폴더에 모아서 만듭니다. 공개 릴리스는 같은 도구를 GitHub Actions가 실행합니다([docs/releasing.md](releasing.md)).

- Windows: `Multitap-<버전>-windows-x64.exe`, `Multitap-Host-<버전>-windows-x64.exe`와 빌드 영수증 `.exe.json`.
- Android: `Multitap-<버전>-android-<abi>.apk` 3개(arm64-v8a, armeabi-v7a, x86_64)와 `tools/verify-android-release.ps1`이 쓴 `.verification.json`.
- 목록 `multitap-release-input.json`.

목록 JSON 예시(크기·SHA-256은 실제 값):

```json
[
  { "product": "client", "version": "1.1.0", "filename": "Multitap-1.1.0-windows-x64.exe", "size": 1, "sha256": "…", "signature": "…", "changelog": "…" },
  { "product": "host", "version": "1.1.0", "filename": "Multitap-Host-1.1.0-windows-x64.exe", "size": 1, "sha256": "…", "signature": "…" },
  { "product": "android-client", "version": "1.1.0", "abi": "arm64-v8a", "format": "apk", "filename": "Multitap-1.1.0-android-arm64-v8a.apk", "size": 1, "sha256": "…" },
  { "product": "android-client", "version": "1.1.0", "abi": "armeabi-v7a", "format": "apk", "filename": "Multitap-1.1.0-android-armeabi-v7a.apk", "size": 1, "sha256": "…" },
  { "product": "android-client", "version": "1.1.0", "abi": "x86_64", "format": "apk", "filename": "Multitap-1.1.0-android-x86_64.apk", "size": 1, "sha256": "…" }
]
```

기존 `server/release-bundle/`을 다른 이름으로 옮긴 뒤 실행합니다. 도구는 기존 번들을 덮어쓰지 않습니다.

```sh
node tools/release/github-release-bundle.mjs bundle <모은 폴더> server/release-bundle
```

도구가 확인하는 것:

- Windows EXE: 크기와 SHA-256, 빌드 영수증의 내장 파일 목록, 그리고 소스에 고정된 공개키로 릴리스 서명을 검사합니다. 직접 빌드한 설치 파일은 자신의 키로 서명하고 고정 공개키를 바꿔야 통과합니다.
- APK: 크기와 SHA-256, 검증 영수증의 서명·내용 검사 결과와 서명자.
- 번들: 클라이언트, Host, Android 세 ABI가 모두 있어야 하고 APK 서명자는 하나여야 합니다. 출력 폴더는 새 폴더만 받으며 기존 번들을 덮어쓰지 않습니다.

번들에는 설치 파일, 고정 필드만 남긴 영수증(`.receipt.json`), `releases.json`, `bundle.json`만 들어갑니다. 이미지 빌드(`RUN … validate`)와 컨테이너 시작(`seed`)이 모든 파일의 크기, SHA-256, 영수증을 다시 확인합니다. 번들을 손으로 고치면 빌드가 실패합니다. 이 검사는 무결성과 빌드 출처 확인이며 코드 서명 인증서가 아닙니다.

### 시작할 때 볼륨에 더하는 규칙

- 볼륨의 `downloads/`에 없는 릴리스만 더하고, 기존 파일과 목록 항목은 바꾸지 않습니다. 같은 이름에 다른 내용이 있거나 목록이 가득 차면(1024개) 계정 준비 전에 시작을 멈춥니다.
- 공용 `downloads/.publish.lock`으로 다른 배포 도구와 겹치지 않게 합니다. 시작 중 컨테이너가 멈춰 남은 자기 잠금과 임시 파일은 다음 시작이 정리합니다(로그의 `recoveredLeftovers`).
- 다른 도구가 남긴 잠금은 지우지 않고 "Another release publication holds downloads/.publish.lock" 오류로 멈춥니다. 실행 중인 배포 도구가 없다면 지우고 다시 시작합니다.

  ```sh
  docker compose stop broker
  docker compose run --rm --no-deps --entrypoint rm broker /var/lib/multitap/downloads/.publish.lock
  docker compose up -d
  ```

## 데이터·권한

- 컨테이너는 UID/GID `1000:1000`으로 실행하고, 루트 파일시스템은 읽기 전용, Linux capability는 모두 제거, `no-new-privileges`를 씁니다. 쓰기 위치는 데이터 볼륨과 작은 `/tmp`뿐입니다.
- 데이터 볼륨에는 SQLite DB와 WAL, `vault.key`, 초기 계정 안내 파일, `downloads/`가 있습니다. Proxmox 토큰은 DB 안에서 `vault.key`로 암호화되므로 **DB와 `vault.key`를 함께 백업**합니다.
- 한 데이터 볼륨을 두 브로커 컨테이너가 동시에 쓰지 않습니다. 같은 서버에 두 번째 설치를 만들려면 다른 폴더에서 `MULTITAP_DATA_VOLUME`과 포트를 바꿉니다.
- bind mount를 쓰려면 새 전용 디렉터리를 `1000:1000`, 0700으로 준비합니다. 컨테이너가 소유권을 바꾸지 않습니다.
- 헬스체크는 `/healthz`의 서비스 이름, 상태, 인증 방식, 이미지 버전 일치를 봅니다. Proxmox 연결이나 원격 화면 검증은 아닙니다.
- 기반 이미지는 `node:24-bookworm-slim@sha256:55fb5f5a…d205`(Docker Hub 공식)로 고정합니다.

## 기존 systemd 설치를 옮길 때

새 볼륨으로 시작하면 새 설치가 됩니다. 기존 계정을 유지하려면 DB와 `vault.key`를 함께 옮깁니다. 실행 중인 SQLite 파일을 그대로 복사하지 말고, 브로커를 멈춘 뒤 데이터 디렉터리 전체를 복사하거나 SQLite backup API를 씁니다. 순서는 백업 → 기존 브로커 중지 → 볼륨 복원과 `1000:1000` 권한 확인 → 컨테이너 시작 → 로그인·Proxmox·배정 확인입니다. 같은 포트의 기존 브로커와 컨테이너를 동시에 실행하지 않습니다. 원본은 확인이 끝날 때까지 둡니다.

## English summary

- **Install:** `cp .env.example .env`, set `MULTITAP_SERVER_ADDRESS=<server IP>`, run `docker compose pull && docker compose up -d`, then open `http://<server IP>:18480` in a browser: the root address leads to the first setup. Until an address is set, that origin serves only the setup pages (no client, Host or download access).
- **First administrator:** sign in with `admin`/`admin`; the setup screen asks for a one-time code that only the server log shows, next to the URL to open: `docker compose logs broker | grep initial_setup` (a new code on every restart).
- **Time zone:** the days of the console's monitoring follow `TZ` in `.env` (an IANA name such as `Europe/Berlin`; empty = UTC). `deploy/docker/install.sh` writes the machine's time zone there when it creates `.env`.
- **Addresses:** the console's Server access page writes the `.env` lines (Compose has no root updater to apply them); change them and run `docker compose up -d`. HTTPS: `MULTITAP_PUBLIC_ORIGIN=https://<domain>`; with a reverse proxy on the same machine use `MULTITAP_PUBLISH_PORT=127.0.0.1:18480` and trust the Docker network gateway with `MULTITAP_TRUSTED_PROXIES=<gateway>` (from `docker network inspect multitap_default`), otherwise proxied requests get `403 http_disabled`.
- **HTTP on the LAN (no certificate):** `MULTITAP_HTTP_ORIGIN=http://<server IP>:18480`, `MULTITAP_ALLOW_INSECURE_HTTP=1`, `MULTITAP_PUBLISH_PORT=0.0.0.0:18480` (IPv4 only: with a bare `18480`, Docker's IPv6 proxy hop arrives from the network gateway and can pose as the trusted HTTPS proxy). Works alone or beside the HTTPS address on the same listener (not with `compose.direct-https.yaml`). **No encryption:** passwords, screens and keystrokes travel in clear; use it only on a trusted network and never forward it to the internet. In the Windows client choose `HTTP`, enter `<server IP>:18480`, run the connection test and accept the per-address HTTP prompt.
- **Reverse proxy:** forward HTTPS to `http://127.0.0.1:18480` with `Host`, `X-Forwarded-Proto: https` and WebSocket upgrade headers (nginx example above).
- **UDP media:** the relay listens on UDP 18481 (as on the native install) and Compose publishes it. Open and forward UDP 18481 in the firewall and router, then set `MULTITAP_UDP_HOST_RELAYS` and `MULTITAP_UDP_CLIENT_RELAYS` (e.g. `vdi.example.com:18481`) and `MULTITAP_UDP_MEDIA=on`. Without them, or with UDP blocked, sessions stay on WebSocket (over HTTPS or HTTP).
- **Updates:** the admin console checks the project's signed `server-updates-docker.json` (GitHub Releases, Ed25519, pinned key) and shows the exact commands; it never pulls or restarts by itself. `MULTITAP_DOCKER_UPDATE_FEED=off` disables the outbound check. Feed: `https://github.com/totblabs/Multitap/releases/latest/download/server-updates-docker.json`. Images: built by GitHub Actions for linux/amd64 on every GitHub Release and pushed as `ghcr.io/totblabs/multitap-broker:<version>` and `:latest` ([docs/releasing.md](releasing.md)). Data, `vault.key` and downloads live in the `multitap-broker-data` volume; never use `down -v`.
- **Build from source:** `docker compose -f compose.yaml -f compose.build.yaml up -d --build`, with a prepared bundle in `server/release-bundle/` (the release's `multitap-release-bundle-<version>.tar.gz`: `rm -rf server/release-bundle && mkdir server/release-bundle && tar -xzf multitap-release-bundle-<version>.tar.gz -C server/release-bundle`), or an empty development bundle and `MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=true`.
- **CI smoke test:** `tools/oss/docker-smoke.sh` builds two images, runs them through `compose.yaml` on a private project and volume, and checks the first setup by IP from the root URL, the LAN HTTP address beside an HTTPS one (the client's HTTP health profile), health, the catalog (client, Host, Android), `/get/*` links and that an image update keeps every earlier file.
