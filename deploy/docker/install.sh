#!/usr/bin/env bash
# Multitap server: install with Docker on Debian or Ubuntu (a Proxmox CT or VM).
#
#   curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | MULTITAP_LANG=ko bash
#
# What it does: installs Docker if it is missing, writes /opt/multitap
# (compose.yaml, .env), starts the server, prints the address and the one-time
# setup code, and installs the `multitap` command (update, status, logs,
# backup, restore, uninstall). Run again later, it changes nothing that is installed.
#
# Settings (environment variables, all optional):
#   MULTITAP_LANG=ko|en      the language of these messages (default: the
#                            locale, LC_ALL / LC_MESSAGES / LANG; Korean when it
#                            starts with ko, else English). Kept in .env, so the
#                            multitap command answers in the same language.
#   MULTITAP_VERSION=1.2.3   this release instead of the newest one
#   MULTITAP_DIR=/opt/multitap
#   MULTITAP_LAN_HTTP=1|0    open (1) or not (0) http://<this server>:18480 for
#                            apps and Hosts without asking (default: ask, Enter = yes)
#   MULTITAP_PUBLIC_HTTP=1|0 also open http://<public IP>:18480 and follow the
#                            public IP when it changes (default: ask, Enter = yes;
#                            without a terminal it stays off unless this is 1).
#                            That address is not encrypted over the internet.
#   MULTITAP_LOCAL=<folder>  test before a release: compose.yaml, .env.example
#                            and multitap from this folder, and the image that
#                            is already loaded in Docker (nothing downloaded)
#
# A Proxmox CT needs Options > Features > nesting (and keyctl for an
# unprivileged CT) for Docker to run in it.
set -euo pipefail

REPOSITORY=totblabs/Multitap
IMAGE_DEFAULT=ghcr.io/totblabs/multitap-broker

# Language (docs/i18n.md): MULTITAP_LANG, else the locale; Korean when it
# starts with ko, English otherwise. t <korean> <english> prints one of them;
# pf <korean format> <english format> <arguments...> is printf with one of them.
case "${MULTITAP_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ko*|KO*) ML=ko ;; *) ML=en ;; esac
t() { if [ "$ML" = ko ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }
# shellcheck disable=SC2059
pf() { local ko=$1 en=$2; shift 2; if [ "$ML" = ko ]; then printf "$ko" "$@"; else printf "$en" "$@"; fi; }

# Colours only on a terminal (curl … | bash keeps stdout on the terminal),
# never with NO_COLOR or a dumb terminal.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != dumb ]; then
  B=$'\033[1m' DIM=$'\033[2m' RED=$'\033[1;31m' GREEN=$'\033[1;32m' YELLOW=$'\033[1;33m' CYAN=$'\033[1;36m' OFF=$'\033[0m'
else
  B='' DIM='' RED='' GREEN='' YELLOW='' CYAN='' OFF=''
fi
STEPS=4
step() { printf '\n%s[%s/%s] %s%s\n' "$CYAN" "$1" "$STEPS" "$2" "$OFF"; }
ok() { printf '  %s✔%s %s\n' "$GREEN" "$OFF" "$*"; }
warn() { printf '  %s⚠%s %s\n' "$YELLOW" "$OFF" "$*"; }
note() { printf '    %s%s%s\n' "$DIM" "$*" "$OFF"; }
say() { printf '  %s•%s %s\n' "$CYAN" "$OFF" "$*"; }
fail() { printf '\n  %s✖ %s%s\n    %s\n' "$RED" "$(t '멈췄어요' 'Stopped')" "$OFF" "$*" >&2; exit 1; }

check_system() {
  [ "$(id -u)" = 0 ] || fail "$(t "root로 실행해 주세요. (sudo bash 또는 root 셸)" "Run this as root (sudo bash, or a root shell).")"
  local arch
  arch=$(uname -m)
  [ "$arch" = x86_64 ] || fail "$(t "x86_64(amd64)에서만 설치할 수 있어요. 지금: $arch" "Multitap installs only on x86_64 (amd64). This machine: $arch")"
  [ -r /etc/os-release ] || fail "$(t "운영체제를 확인하지 못했어요. Debian이나 Ubuntu에서 실행해 주세요." "Could not identify the operating system. Run this on Debian or Ubuntu.")"
  # shellcheck disable=SC1091
  . /etc/os-release
  case "${ID:-} ${ID_LIKE:-}" in
    *debian*|*ubuntu*) ;;
    *) fail "$(t "Debian이나 Ubuntu에서만 설치할 수 있어요. 지금: ${PRETTY_NAME:-알 수 없음}" "Multitap installs only on Debian or Ubuntu. This system: ${PRETTY_NAME:-unknown}")" ;;
  esac
  OS_ID=${ID}
  OS_CODENAME=${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}
  IN_CT=no
  if command -v systemd-detect-virt >/dev/null 2>&1 && [ "$(systemd-detect-virt -c 2>/dev/null || true)" = lxc ]; then IN_CT=yes; fi
}

docker_version() { docker --version | sed 's/,.*//; s/Docker version //'; }

install_docker() {
  if docker compose version >/dev/null 2>&1; then
    local have
    have=$(docker_version)
    ok "$(t "Docker가 이미 있어요 ($have)" "Docker is already installed ($have)")"
    return
  fi
  say "$(t "Docker를 설치해요. 1~2분 걸려요." "Installing Docker. This takes 1 to 2 minutes.")"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq ca-certificates curl gnupg >/dev/null
  local repo_os=debian
  [ "$OS_ID" = ubuntu ] && repo_os=ubuntu
  install -m 0755 -d /etc/apt/keyrings
  if [ -n "$OS_CODENAME" ] && curl -fsSL "https://download.docker.com/linux/$repo_os/gpg" -o /etc/apt/keyrings/docker.asc; then
    chmod a+r /etc/apt/keyrings/docker.asc
    echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/$repo_os $OS_CODENAME stable" \
      > /etc/apt/sources.list.d/docker.list
    if apt-get update -qq && apt-get install -y -qq docker-ce docker-ce-cli containerd.io docker-compose-plugin >/dev/null; then
      :
    else
      note "$(t "Docker 공식 저장소에서 받지 못해 배포판 패키지로 설치해요." "Could not get Docker from its official repository; installing the distribution's package instead.")"
      rm -f /etc/apt/sources.list.d/docker.list
      apt-get update -qq
      apt-get install -y -qq docker.io docker-compose >/dev/null
    fi
  else
    apt-get install -y -qq docker.io docker-compose >/dev/null
  fi
  systemctl enable --now docker >/dev/null 2>&1 || true
  docker compose version >/dev/null 2>&1 || fail "$(t "Docker Compose를 설치하지 못했어요." "Could not install Docker Compose.")"
  local have
  have=$(docker_version)
  ok "$(t "Docker를 설치했어요 ($have)" "Docker installed ($have)")"
}

newest_version() {
  local url
  url=$(curl -fsSI -o /dev/null -w '%{redirect_url}' "https://github.com/$REPOSITORY/releases/latest" || true)
  case "$url" in
    */tag/v[0-9]*) printf '%s\n' "${url##*/tag/v}" ;;
    *) fail "$(t "새 버전을 확인하지 못했어요. 인터넷 연결을 확인하거나 MULTITAP_VERSION=1.0.0 처럼 버전을 정해 주세요." "Could not find the newest release. Check the internet connection, or choose a version, e.g. MULTITAP_VERSION=1.0.0.")" ;;
  esac
}

server_address() {
  local address
  address=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit }}' || true)
  [ -n "$address" ] || address=$(hostname -I 2>/dev/null | awk '{print $1}')
  printf '%s\n' "$address"
}

# This machine's time zone as an IANA name (Europe/Berlin), or nothing when
# it cannot be told: timedatectl, then /etc/timezone, then where
# /etc/localtime points. The container runs in UTC unless .env names a zone.
zone_name() { # zone_name <text>: true for a zone name this system knows
  case "$1" in ''|/*|*[!A-Za-z0-9_+/-]*) return 1 ;; esac
  [ ! -d /usr/share/zoneinfo ] || [ -f "/usr/share/zoneinfo/$1" ]
}
host_timezone() {
  local zone=''
  if command -v timedatectl >/dev/null 2>&1; then
    zone=$(timedatectl show -p Timezone --value 2>/dev/null || true)
    zone_name "$zone" || zone=''
  fi
  if [ -z "$zone" ] && [ -r /etc/timezone ]; then
    zone=$( { head -n 1 /etc/timezone | tr -d '[:space:]'; } 2>/dev/null || true)
    zone_name "$zone" || zone=''
  fi
  if [ -z "$zone" ] && [ -L /etc/localtime ]; then
    zone=$( { readlink -f /etc/localtime | sed -n 's|^.*/zoneinfo/||; s|^posix/||; p'; } 2>/dev/null || true)
    zone_name "$zone" || zone=''
  fi
  [ -z "$zone" ] || printf '%s\n' "$zone"
}

fetch() { # fetch <repository path> <destination>
  if [ -n "${MULTITAP_LOCAL:-}" ]; then
    cp "$MULTITAP_LOCAL/$(basename "$1")" "$2"
  else
    curl -fsSL "https://raw.githubusercontent.com/$REPOSITORY/v$VERSION/$1" -o "$2" || fail "$(t "$1 파일을 받지 못했어요." "Could not download $1.")"
  fi
}

set_env() { # set_env <name> <value>: replaces the line, or adds it
  if grep -q "^$1=" "$DIR/.env"; then
    sed -i "s|^$1=.*|$1=$2|" "$DIR/.env"
  else
    printf '%s=%s\n' "$1" "$2" >> "$DIR/.env"
  fi
}

compose() { (cd "$DIR" && docker compose "$@"); }

# The server is up when it answers at its own address (it refuses 127.0.0.1)
# with the expected version. A container that keeps restarting is a failure
# at once, not after the whole wait.
served_version() {
  curl -fs --max-time 2 -H 'Accept: application/json' "http://$1:18480/healthz" 2>/dev/null \
    | grep -o '"version":"[^"]*"' | head -n 1 | cut -d'"' -f4
}
wait_version() { # wait_version <address> <version>
  local i id restarts
  for i in $(seq 1 90); do
    [ "$(served_version "$1")" = "$2" ] && return 0
    id=$(compose ps -aq broker 2>/dev/null | head -n 1 || true)
    if [ -n "$id" ]; then
      restarts=$(docker inspect -f '{{.RestartCount}}' "$id" 2>/dev/null || echo 0)
      [ "${restarts:-0}" -ge 2 ] && return 1
    fi
    sleep 2
  done
  return 1
}
setup_code() {
  # No code in the log (setup already done) is an answer, not a failure under pipefail.
  { compose logs --no-color broker 2>/dev/null | grep -o '"setupCode":"[^"]*"' | tail -n 1 | sed 's/.*:"\(.*\)"/\1/'; } || true
}

main() {
  DIR=${MULTITAP_DIR:-/opt/multitap}
  printf '\n%s▶ %s%s\n' "$B" "$(t 'Multitap 서버 설치' 'Installing the Multitap server')" "$OFF"
  step 1 "$(t "시스템 확인" "Checking the system")"
  check_system
  ok "${PRETTY_NAME:-Linux}, $(uname -m)"
  # Finished installs carry this mark; an install that stopped half way, or
  # one removed with 'multitap uninstall', is taken up again with its .env.
  if [ -f "$DIR/.installed" ] && [ -x /usr/local/bin/multitap ]; then
    ok "$(t "이미 설치돼 있어요 ($DIR). 아무것도 바꾸지 않았어요." "Already installed ($DIR). Nothing was changed.")"
    pf '  %s→%s 새 버전: %smultitap update%s   상태: %smultitap status%s\n\n' \
       '  %s→%s New version: %smultitap update%s   Status: %smultitap status%s\n\n' "$CYAN" "$OFF" "$B" "$OFF" "$B" "$OFF"
    exit 0
  fi
  KEEP_ENV=no
  [ -f "$DIR/.env" ] && KEEP_ENV=yes
  if [ "$IN_CT" = yes ]; then
    ok "$(t "Proxmox CT예요" "Running in a Proxmox CT")"
    note "$(t "CT 옵션 > 기능에서 nesting이 켜져 있어야 Docker가 돌아요." "Docker needs nesting turned on under the CT's Options > Features.")"
  fi

  step 2 "Docker"
  install_docker

  step 3 "$(t "서버 파일과 이미지" "Server files and image")"
  if [ -n "${MULTITAP_VERSION:-}" ]; then
    VERSION=$MULTITAP_VERSION
  elif [ "$KEEP_ENV" = yes ] && grep -qE '^MULTITAP_VERSION=[0-9]' "$DIR/.env"; then
    VERSION=$(grep -E '^MULTITAP_VERSION=' "$DIR/.env" | tail -n 1 | cut -d= -f2-)
  elif [ -n "${MULTITAP_LOCAL:-}" ]; then
    [ -f "$MULTITAP_LOCAL/compose.yaml" ] || fail "$(t "$MULTITAP_LOCAL 에 compose.yaml이 없어요." "No compose.yaml in $MULTITAP_LOCAL.")"
    VERSION=$(sed -n 's/.*MULTITAP_VERSION:-\([0-9][0-9.]*\)}.*/\1/p' "$MULTITAP_LOCAL/compose.yaml" | head -n 1)
  else
    VERSION=$(newest_version)
  fi
  case "$VERSION" in [0-9]*.[0-9]*.[0-9]*) ;; *) fail "$(t "버전을 알 수 없어요: $VERSION" "Unknown version: $VERSION")" ;; esac

  install -d -m 0750 "$DIR" "$DIR/backups"
  chmod 0700 "$DIR/backups"
  fetch compose.yaml "$DIR/compose.yaml"
  fetch .env.example "$DIR/.env.example"
  fetch deploy/docker/multitap /usr/local/bin/multitap
  chmod 0755 /usr/local/bin/multitap
  if [ "$KEEP_ENV" = yes ]; then
    ADDRESS=$(grep -E '^MULTITAP_SERVER_ADDRESS=' "$DIR/.env" | tail -n 1 | cut -d= -f2- || true)
    [ -n "$ADDRESS" ] || ADDRESS=$(server_address)
    ok "$(t "남아 있던 설정과 데이터로 이어서 설치해요 ($DIR)" "Continuing with the settings and data already there ($DIR)")"
  else
    cp "$DIR/.env.example" "$DIR/.env"
    ADDRESS=$(server_address)
    [ -n "$ADDRESS" ] && set_env MULTITAP_SERVER_ADDRESS "$ADDRESS"
    ok "$(t "설정 파일을 만들었어요 ($DIR)" "Settings file created ($DIR)")"
    # The server's days (monitoring) follow this machine's time zone; an
    # unknown zone leaves the line empty (UTC).
    local zone
    zone=$(host_timezone)
    if [ -n "$zone" ]; then
      set_env TZ "$zone"
      note "$(t "시간대: $zone (바꾸려면 $DIR/.env 의 TZ)" "Time zone: $zone (to change it: TZ in $DIR/.env)")"
    fi
  fi
  chmod 0600 "$DIR/.env"
  set_env MULTITAP_VERSION "$VERSION"
  # The multitap command answers in the language of the install.
  set_env MULTITAP_LANG "$ML"

  # Apps and Hosts on the same network connect to this address. Without it a
  # fresh server answers only its first-setup page, and no Host can connect
  # until 'multitap address' is run.
  if [ -n "$ADDRESS" ] && ! grep -qE '^MULTITAP_HTTP_ORIGIN=.+' "$DIR/.env"; then
    local lan=${MULTITAP_LAN_HTTP:-ask} answer=''
    if [ "$lan" = ask ]; then
      if [ -r /dev/tty ] && [ -w /dev/tty ]; then
        pf '\n  %s?%s %s앱과 Host가 이 주소로 접속하게 할까요?%s  %shttp://%s:18480%s\n' \
           '\n  %s?%s %sLet apps and Hosts connect at this address?%s  %shttp://%s:18480%s\n' "$YELLOW" "$OFF" "$B" "$OFF" "$CYAN" "$ADDRESS" "$OFF" > /dev/tty
        pf '    %s같은 네트워크용이에요. 암호화되지 않아요. 나중에 바꾸려면 multitap address%s\n    [Y/n]: ' \
           '    %sFor the same network. Not encrypted. To change it later: multitap address%s\n    [Y/n]: ' "$DIM" "$OFF" > /dev/tty
        read -r answer < /dev/tty || answer=''
      fi
      case "$answer" in n|N|no|No) lan=0 ;; *) lan=1 ;; esac
    fi
    if [ "$lan" = 1 ]; then
      set_env MULTITAP_HTTP_ORIGIN "http://$ADDRESS:18480"
      set_env MULTITAP_ALLOW_INSECURE_HTTP 1
      set_env MULTITAP_PUBLISH_PORT "0.0.0.0:18480"
      ok "$(t "앱·Host 접속 주소: http://$ADDRESS:18480" "Address for apps and Hosts: http://$ADDRESS:18480")"
      # From outside too: the public IP, followed when the provider changes it
      # (nobody will run a command for this).
      local public='' outside=${MULTITAP_PUBLIC_HTTP:-ask} reply=''
      public=$( { curl -fs --max-time 5 https://1.1.1.1/cdn-cgi/trace 2>/dev/null | sed -n 's/^ip=//p'; } || true)
      [[ $public =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || public=''
      if [ -n "$public" ] && [ "$public" != "$ADDRESS" ] && [ "$outside" != 0 ]; then
        if [ "$outside" = ask ] && [ -r /dev/tty ] && [ -w /dev/tty ]; then
          pf '\n  %s?%s %s밖에서도(공유기 포트 포워딩) 접속하게 할까요?%s  %shttp://%s:18480%s\n' \
             '\n  %s?%s %sAllow connections from outside too (router port forwarding)?%s  %shttp://%s:18480%s\n' "$YELLOW" "$OFF" "$B" "$OFF" "$CYAN" "$public" "$OFF" > /dev/tty
          pf '    %s%s인터넷을 지날 때 암호화되지 않아요. 밖에서 쓰려면 HTTPS 주소를 권해요.%s\n' \
             '    %s%sNot encrypted over the internet. For use from outside, an HTTPS address is recommended.%s\n' "$YELLOW" "$B" "$OFF" > /dev/tty
          pf '    %s이 공인 IP는 자동으로 알아냈어요. 바뀌면 10분 안에 알아서 따라가요.%s\n' \
             '    %sThis public IP was found automatically. If it changes, Multitap follows it within 10 minutes.%s\n' "$DIM" "$OFF" > /dev/tty
          pf '    %s공유기에서 TCP 18480, UDP 18481을 이 서버로 포트 포워딩해야 밖에서 들어와요.%s\n' \
             '    %sConnections from outside arrive only after your router forwards TCP 18480 and UDP 18481 to this server.%s\n' "$DIM" "$OFF" > /dev/tty
          pf '    %s아직 안 했어도 켜 두면 돼요. 같은 네트워크는 그대로 되고, 포워딩하는 순간부터 밖에서도 돼요.%s\n    [Y/n]: ' \
             '    %sYou can turn it on before that. The same network keeps working, and outside access starts as soon as you forward the ports.%s\n    [Y/n]: ' "$DIM" "$OFF" > /dev/tty
          read -r reply < /dev/tty || reply=''
          case "$reply" in n|N|no|No) outside=0 ;; *) outside=1 ;; esac
        elif [ "$outside" = ask ]; then
          # Nobody to ask: an unencrypted address on the internet is never
          # opened silently. MULTITAP_PUBLIC_HTTP=1 opens it without asking.
          outside=0
          note "$(t "밖에서 접속(http://$public:18480)은 꺼 두었어요. 켜려면 multitap public-ip on" "Access from outside (http://$public:18480) is left off. To turn it on: multitap public-ip on")"
        fi
        if [ "$outside" = 1 ]; then
          set_env MULTITAP_HTTP_ORIGIN "http://$ADDRESS:18480,http://$public:18480"
          printf '%s\n' "$public" > "$DIR/public-ip"
          FOLLOW_PUBLIC=yes
          ok "$(t "밖에서 접속할 주소: http://$public:18480 (공인 IP가 바뀌면 따라가요)" "Address from outside: http://$public:18480 (follows the public IP when it changes)")"
          warn "$(t "인터넷을 지날 때 암호화되지 않아요. 밖에서 쓰려면 HTTPS 주소를 권해요." "Not encrypted over the internet. For use from outside, an HTTPS address is recommended.")"
        fi
      fi
    else
      note "$(t "앱·Host 접속 주소는 나중에 정해요: multitap address http://$ADDRESS:18480" "Set the address for apps and Hosts later: multitap address http://$ADDRESS:18480")"
    fi
  fi

  local image
  image=$(grep -E '^MULTITAP_IMAGE=' "$DIR/.env" | cut -d= -f2- || true)
  image=${image:-$IMAGE_DEFAULT}
  if docker image inspect "$image:$VERSION" >/dev/null 2>&1; then
    ok "$(t "서버 이미지($VERSION)가 이미 있어요" "Server image $VERSION is already here")"
  elif [ -n "${MULTITAP_LOCAL:-}" ]; then
    # A saved image beside the files (docker save | gzip): installs without the internet registry.
    local saved
    saved=$(ls "$MULTITAP_LOCAL"/multitap-broker-"$VERSION".tar* 2>/dev/null | head -n 1 || true)
    [ -n "$saved" ] || fail "$(t "$MULTITAP_LOCAL 에 multitap-broker-$VERSION.tar.gz 이미지 파일이 없어요." "No image file multitap-broker-$VERSION.tar.gz in $MULTITAP_LOCAL.")"
    say "$(t "서버 이미지를 불러와요." "Loading the server image.")"
    docker load -q -i "$saved" >/dev/null || fail "$(t "이미지 파일을 불러오지 못했어요: $saved" "Could not load the image file: $saved")"
    docker image inspect "$image:$VERSION" >/dev/null 2>&1 || fail "$(t "불러온 파일에 $image:$VERSION 이미지가 없어요." "The loaded file has no $image:$VERSION image.")"
    ok "$(t "서버 이미지($VERSION)를 불러왔어요" "Server image $VERSION loaded")"
  else
    say "$(t "서버 이미지를 받아요." "Downloading the server image.")"
    compose pull --quiet >/dev/null 2>&1 || fail "$(t "서버 이미지($image:$VERSION)를 받지 못했어요. 인터넷 연결을 확인해 주세요." "Could not download the server image $image:$VERSION. Check the internet connection.")"
    ok "$(t "서버 이미지($VERSION)를 받았어요" "Server image $VERSION downloaded")"
  fi

  step 4 "$(t "서버 시작" "Starting the server")"
  # A CT without nesting starts no container at all: say so plainly.
  if ! docker run --rm --network none --entrypoint true "$image:$VERSION" >/dev/null 2>&1; then
    if [ "$IN_CT" = yes ]; then
      fail "$(t "이 CT에서 Docker 컨테이너를 실행하지 못했어요. Proxmox에서 CT > 옵션 > 기능의 nesting(권한 없는 CT는 keyctl도)을 켜고 CT를 다시 시작한 뒤 이 명령을 다시 실행해 주세요." "Docker could not run a container in this CT. In Proxmox, turn on nesting under CT > Options > Features (and keyctl for an unprivileged CT), restart the CT, then run this command again.")"
    fi
    fail "$(t "Docker 컨테이너를 실행하지 못했어요. 'docker run hello-world'로 Docker가 되는지 확인해 주세요." "Docker could not run a container. Check Docker with 'docker run hello-world'.")"
  fi
  local started
  started=$(compose up -d --quiet-pull 2>&1) || { printf '%s\n' "$started" >&2; fail "$(t "서버를 시작하지 못했어요." "Could not start the server.")"; }
  say "$(t "서버가 응답하기를 기다려요." "Waiting for the server to answer.")"
  if ! wait_version "$ADDRESS" "$VERSION"; then
    compose logs --no-color --tail 40 broker >&2 || true
    fail "$(t "서버가 3분 안에 응답하지 않았어요. 위 기록을 확인해 주세요. 다시 보려면: multitap logs" "The server did not answer within 3 minutes. Check the log above. To see it again: multitap logs")"
  fi
  ok "$(t "서버($VERSION)가 응답해요" "Server $VERSION is answering")"

  touch "$DIR/.installed"
  if [ "${FOLLOW_PUBLIC:-no}" = yes ]; then /usr/local/bin/multitap public-ip on >/dev/null 2>&1 || warn "$(t "공인 IP 자동 따라가기를 켜지 못했어요. 나중에: multitap public-ip on" "Could not turn on public IP following. Later: multitap public-ip on")"; fi
  local code line here
  code=$(setup_code)
  here=${ADDRESS:-$(t '<이 서버 IP>' "<this server's IP>")}
  line='━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━'
  printf '\n%s%s%s\n' "$GREEN" "$line" "$OFF"
  pf ' %s✔ 설치를 마쳤어요%s  (Multitap 서버 %s)\n' \
     ' %s✔ Installed%s  (Multitap server %s)\n' "$GREEN" "$OFF" "$VERSION"
  printf '%s%s%s\n\n' "$GREEN" "$line" "$OFF"
  pf '  %s①%s 브라우저로 열기   %shttp://%s:18480%s\n' \
     '  %s①%s Open in a browser   %shttp://%s:18480%s\n' "$CYAN" "$OFF" "$B" "$here" "$OFF"
  if [ -n "$code" ]; then
    pf '  %s②%s 처음 로그인       %sadmin%s / %sadmin%s\n' \
       '  %s②%s First sign-in       %sadmin%s / %sadmin%s\n' "$CYAN" "$OFF" "$B" "$OFF" "$B" "$OFF"
    pf '  %s③%s 설정 코드         %s%s%s\n' \
       '  %s③%s Setup code          %s%s%s\n' "$CYAN" "$OFF" "$YELLOW" "$code" "$OFF"
    pf '  %s→%s 그다음 새 관리자 아이디와 비밀번호를 정하고, Proxmox를 연결해요.\n' \
       '  %s→%s Then choose a new admin ID and password, and connect Proxmox.\n' "$CYAN" "$OFF"
  else
    pf '  %s②%s 처음 설정을 이미 마친 서버예요. 쓰던 관리자 계정으로 로그인해요.\n' \
       '  %s②%s First setup is already done on this server. Sign in with your admin account.\n' "$CYAN" "$OFF"
  fi
  if grep -qE '^MULTITAP_HTTP_ORIGIN=.+' "$DIR/.env"; then
    pf '  %s→%s 앱과 Host에는 %shttp://%s:18480%s 를 넣어요.\n' \
       '  %s→%s In apps and Hosts, enter %shttp://%s:18480%s\n' "$CYAN" "$OFF" "$B" "$here" "$OFF"
    [ -s "$DIR/public-ip" ] && pf '  %s→%s 밖에서는 %shttp://%s:18480%s\n' \
       '  %s→%s From outside: %shttp://%s:18480%s\n' "$CYAN" "$OFF" "$B" "$(cat "$DIR/public-ip")" "$OFF"
  else
    pf '  %s→%s 앱과 Host가 붙기 전에: %smultitap address http://%s:18480%s\n' \
       '  %s→%s Before apps and Hosts can connect: %smultitap address http://%s:18480%s\n' "$CYAN" "$OFF" "$B" "$here" "$OFF"
  fi
  # From outside, in three lines.
  pf '\n  %s밖에서 접속하려면%s\n' '\n  %sTo connect from outside%s\n' "$B" "$OFF"
  local target=${ADDRESS:-$(t '이 서버' 'this server')}
  if [ -s "$DIR/public-ip" ]; then
    pf '  %s✔%s 공인 IP %s 를 등록했고(자동으로 알아냄), 바뀌면 알아서 따라가요.\n' \
       '  %s✔%s Public IP %s is registered (found automatically) and followed when it changes.\n' "$GREEN" "$OFF" "$(cat "$DIR/public-ip")"
    pf '  %s1%s 공유기 포트 포워딩: TCP 18480, UDP 18481 → %s\n' \
       '  %s1%s Router port forwarding: TCP 18480, UDP 18481 → %s\n' "$CYAN" "$OFF" "$target"
    pf '    %s포워딩을 안 하면 밖에서는 안 들어와요. 같은 네트워크는 그대로 돼요.%s\n' \
       '    %sWithout forwarding nothing comes in from outside. The same network keeps working.%s\n' "$DIM" "$OFF"
  else
    pf '  %s1%s 공유기 포트 포워딩: TCP 18480, UDP 18481 → %s\n' \
       '  %s1%s Router port forwarding: TCP 18480, UDP 18481 → %s\n' "$CYAN" "$OFF" "$target"
    pf '  %s2%s 이 서버에서 %smultitap public-ip on%s  (공인 IP를 자동으로 알아내 등록하고, 바뀌면 따라가요)\n' \
       '  %s2%s On this server: %smultitap public-ip on%s  (finds and registers the public IP, and follows it when it changes)\n' "$CYAN" "$OFF" "$B" "$OFF"
  fi
  pf '    %s도메인이 있으면 HTTPS가 더 안전해요: 관리자 페이지 > 서버 접속 설정%s\n' \
     '    %sWith a domain, HTTPS is safer: admin console > Server access%s\n' "$DIM" "$OFF"
  pf '\n  %s⚠%s 방화벽이 있으면 18480(TCP)과 18481(UDP, 화면 전송)을 열어 주세요.\n' \
     '\n  %s⚠%s If there is a firewall, open 18480 (TCP) and 18481 (UDP, screen streaming).\n' "$YELLOW" "$OFF"
  pf '  %s관리 명령: multitap status · logs · update · backup · restore · code · address · public-ip · uninstall%s\n\n' \
     '  %sCommands: multitap status · logs · update · backup · restore · code · address · public-ip · uninstall%s\n\n' "$DIM" "$OFF"
}
main "$@"
