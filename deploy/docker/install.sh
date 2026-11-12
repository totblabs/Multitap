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
# The addresses apps and Hosts use are set in the admin console (Server
# access) and kept by the server in its data volume. The two questions here
# only give a new server its first addresses.
#
# What it prints is also kept in /var/log/multitap-install.log (root only),
# without the setup code.
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
#   MULTITAP_PUBLIC_HTTP=1|0 also open http://<public IP>:18480 and let the server
#                            follow the public IP when it changes (default: ask,
#                            Enter = no; without a terminal it stays off unless
#                            this is 1). That address is not encrypted over the
#                            internet.
#   MULTITAP_CT_CHECK=0      skip the check of the Proxmox CT's nesting feature
#   MULTITAP_LOCAL=<folder>  test before a release: compose.yaml, .env.example
#                            and multitap from this folder, and the image that
#                            is already loaded in Docker (nothing downloaded)
#
# A Proxmox CT needs Options > Features > nesting (and keyctl for an
# unprivileged CT) for Docker to run in it.
set -Eeuo pipefail

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
STEP_NAME=''
step() { STEP_NAME=$2; printf '\n%s[%s/%s] %s%s\n' "$CYAN" "$1" "$STEPS" "$2" "$OFF"; }
ok() { printf '  %s✔%s %s\n' "$GREEN" "$OFF" "$*"; }
warn() { printf '  %s⚠%s %s\n' "$YELLOW" "$OFF" "$*"; }
note() { printf '    %s%s%s\n' "$DIM" "$*" "$OFF"; }
say() { printf '  %s•%s %s\n' "$CYAN" "$OFF" "$*"; }

# ---- the record of this run -------------------------------------------------
# From start_log on, what is printed also goes to $LOG, without colours, and
# the output of the quiet steps (apt, docker) goes there instead of nowhere.
# The setup code never does: the closing lines are printed after stop_log, and
# the server's log is shown without the line that carries the code.
LOG=/dev/null
LOG_PID=''
start_log() {
  local file=/var/log/multitap-install.log
  if ! ( umask 077; : >> "$file" ) 2>/dev/null; then return 0; fi
  chmod 0600 "$file" 2>/dev/null || true
  LOG=$file
  printf '\n==== install.sh %s ====\n' "$(date '+%Y-%m-%d %H:%M:%S %z')" >> "$LOG"
  exec 3>&1 4>&2
  # Each line goes to the screen as it is and to the record without this
  # script's own colour codes.
  exec > >(
    while IFS= read -r line || [ -n "$line" ]; do
      printf '%s\n' "$line" >&3
      for code in "$B" "$DIM" "$RED" "$GREEN" "$YELLOW" "$CYAN" "$OFF"; do [ -z "$code" ] || line=${line//"$code"/}; done
      printf '%s\n' "$line" >> "$LOG"
    done
  ) 2>&1
  LOG_PID=$!
}
stop_log() { # the screen only from here on; the record is complete when this returns
  [ -n "$LOG_PID" ] || return 0
  exec 1>&3 2>&4 3>&- 4>&-
  wait "$LOG_PID" 2>/dev/null || true
  LOG_PID=''
}
# A quiet step: its output is kept in the record, and shown when the step
# fails. LOGGED names the step for that message while it has not succeeded.
LOGGED=''
logged() {
  local status=0
  LOGGED=$*
  if [ "$LOG" = /dev/null ]; then "$@" > /dev/null || status=$?; else "$@" >> "$LOG" 2>&1 || status=$?; fi
  [ "$status" -eq 0 ] || return "$status"
  LOGGED=''
}
log_hint() { [ "$LOG" = /dev/null ] || printf '    %s\n' "$(t "설치 기록: $LOG" "Install log: $LOG")" >&2; }
fail() {
  stop_log
  printf '\n  %s✖ %s%s\n    %s\n' "$RED" "$(t '멈췄어요' 'Stopped')" "$OFF" "$*" >&2
  [ "$LOG" = /dev/null ] || printf 'STOPPED%s: %s\n' "${STEP_NAME:+ in [$STEP_NAME]}" "$*" >> "$LOG"
  log_hint
  exit 1
}
# A command that ends the script on its own (set -e) says where and why.
on_error() { # on_error <exit status> <line> <command>
  local status=$1 command=${LOGGED:-$3}
  # Inside $( ) the caller reports.
  [ "$BASH_SUBSHELL" -eq 0 ] || exit "$status"
  trap - ERR
  stop_log
  {
    printf '\n  %s✖ %s%s\n' "$RED" "$(t '멈췄어요' 'Stopped')" "$OFF"
    printf '    %s\n' "$(t "${STEP_NAME:+[$STEP_NAME] 단계에서 }명령이 실패했어요 (줄 $2, 종료 코드 $status): $command" "A command failed${STEP_NAME:+ in step [$STEP_NAME]} (line $2, exit code $status): $command")"
    if [ "$LOG" != /dev/null ]; then
      printf '    %s\n' "$(t "기록의 마지막 줄:" "The last lines of the log:")"
      tail -n 15 "$LOG" | sed 's/^/      /'
    fi
  } >&2
  [ "$LOG" = /dev/null ] || printf 'FAILED%s: line %s, exit code %s: %s\n' "${STEP_NAME:+ in [$STEP_NAME]}" "$2" "$status" "$command" >> "$LOG"
  log_hint
  exit "$status"
}
trap 'on_error "$?" "$LINENO" "$BASH_COMMAND"' ERR
trap stop_log EXIT

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

# Docker in a Proxmox CT needs the CT's nesting feature: without it no
# container starts, which would otherwise show only after Docker is installed
# and the image downloaded. Two signs are read here, before anything is
# installed, and only both together say "off": a CT with nesting has
# /dev/.lxc/proc (LXC mounts it there), and a new proc mount, which Docker
# makes for every container, is refused without it. keyctl cannot be seen from
# inside the CT; the start test after the image download stays as the backstop.
ct_nesting() {
  [ ! -d /dev/.lxc/proc ] || return 0
  local dir mounted=1
  dir=$(mktemp -d 2>/dev/null) || return 0
  if mount -t proc proc "$dir" 2>/dev/null; then mounted=0; umount -l "$dir" 2>/dev/null || true; fi
  rmdir "$dir" 2>/dev/null || true
  return "$mounted"
}
# A CT whose root is mapped to another user on the node (unprivileged).
ct_unprivileged() { [ -r /proc/self/uid_map ] && ! awk 'NR == 1 && $1 == 0 && $2 == 0 { whole = 1 } END { exit !whole }' /proc/self/uid_map; }
ct_features() { if ct_unprivileged; then printf 'nesting=1,keyctl=1'; else printf 'nesting=1'; fi; }
check_ct() {
  [ "$IN_CT" = yes ] || return 0
  if [ "${MULTITAP_CT_CHECK:-1}" != 0 ] && ! ct_nesting; then
    fail "$(t "이 CT는 nesting이 꺼져 있어 Docker 컨테이너를 실행할 수 없어요. 아직 아무것도 설치하지 않았어요.
    Proxmox 웹: 이 CT > 옵션(Options) > 기능(Features) > 편집에서 nesting$(ct_unprivileged && printf '과 keyctl')을 켜고 CT를 다시 시작해요.
    또는 Proxmox 노드의 Shell에서:  pct set <CT 번호> --features $(ct_features) && pct reboot <CT 번호>
    그다음 이 명령을 다시 실행해 주세요. (이 확인을 건너뛰려면 MULTITAP_CT_CHECK=0)" \
               "Nesting is off in this CT, so Docker cannot run containers here. Nothing was installed yet.
    Proxmox web: this CT > Options > Features > Edit: turn on nesting$(ct_unprivileged && printf ' and keyctl'), then restart the CT.
    Or in the Proxmox node's Shell:  pct set <CT number> --features $(ct_features) && pct reboot <CT number>
    Then run this command again. (To skip this check: MULTITAP_CT_CHECK=0)")"
  fi
  ok "$(t "Proxmox CT예요" "Running in a Proxmox CT")"
  if ct_unprivileged; then
    note "$(t "권한 없는 CT예요. CT 옵션 > 기능에서 nesting과 keyctl이 켜져 있어야 Docker가 돌아요." "An unprivileged CT. Docker needs nesting and keyctl turned on under the CT's Options > Features.")"
  else
    note "$(t "CT 옵션 > 기능에서 nesting이 켜져 있어야 Docker가 돌아요." "Docker needs nesting turned on under the CT's Options > Features.")"
  fi
}

docker_version() { docker --version | sed 's/,.*//; s/Docker version //'; }

# The distribution's own packages, when Docker's repository cannot be used.
# `docker compose` is Compose v2 as a plugin of the docker command. Ubuntu
# packages it as docker-compose-v2; where the docker-compose package itself
# is v2 it brings the plugin too. A docker-compose 1.x (Debian 12, Ubuntu
# 22.04 and 24.04) is a separate command this installer cannot use, so it is
# not installed.
install_distribution_docker() {
  FROM_DISTRIBUTION=yes
  logged apt-get install -y -qq docker.io
  if logged apt-get install -y -qq docker-compose-v2; then return 0; fi
  local candidate
  candidate=$( { apt-cache policy docker-compose 2>/dev/null | sed -n 's/^ *Candidate: *//p' | head -n 1; } || true)
  candidate=${candidate#*:}
  case "$candidate" in [2-9].*|[1-9][0-9].*) logged apt-get install -y -qq docker-compose || true ;; esac
}
install_docker() {
  if docker compose version >/dev/null 2>&1; then
    local have
    have=$(docker_version)
    ok "$(t "Docker가 이미 있어요 ($have)" "Docker is already installed ($have)")"
    return
  fi
  say "$(t "Docker를 설치해요. 1~2분 걸려요." "Installing Docker. This takes 1 to 2 minutes.")"
  export DEBIAN_FRONTEND=noninteractive
  FROM_DISTRIBUTION=no
  logged apt-get update -qq
  logged apt-get install -y -qq ca-certificates curl gnupg
  local repo_os=debian
  [ "$OS_ID" = ubuntu ] && repo_os=ubuntu
  install -m 0755 -d /etc/apt/keyrings
  if [ -n "$OS_CODENAME" ] && curl -fsSL "https://download.docker.com/linux/$repo_os/gpg" -o /etc/apt/keyrings/docker.asc; then
    chmod a+r /etc/apt/keyrings/docker.asc
    echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/$repo_os $OS_CODENAME stable" \
      > /etc/apt/sources.list.d/docker.list
    if logged apt-get update -qq && logged apt-get install -y -qq docker-ce docker-ce-cli containerd.io docker-compose-plugin; then
      :
    else
      note "$(t "Docker 공식 저장소에서 받지 못해 배포판 패키지로 설치해요." "Could not get Docker from its official repository; installing the distribution's package instead.")"
      rm -f /etc/apt/sources.list.d/docker.list
      logged apt-get update -qq
      install_distribution_docker
    fi
  else
    install_distribution_docker
  fi
  systemctl enable --now docker >/dev/null 2>&1 || true
  if ! docker compose version >/dev/null 2>&1; then
    if [ "$FROM_DISTRIBUTION" = yes ]; then
      fail "$(t "Docker 공식 저장소(download.docker.com)에서 받지 못했고, 이 배포판의 패키지에는 'docker compose'(Compose v2)가 없어요. download.docker.com 에 연결되는지 확인한 뒤 이 명령을 다시 실행해 주세요." \
                 "Docker could not be downloaded from its official repository (download.docker.com), and this distribution's packages have no 'docker compose' (Compose v2). Check that download.docker.com can be reached, then run this command again.")"
    fi
    fail "$(t "Docker Compose를 설치하지 못했어요." "Could not install Docker Compose.")"
  fi
  local have
  have=$(docker_version)
  ok "$(t "Docker를 설치했어요 ($have)" "Docker installed ($have)")"
}

# The newest release's version; nothing when it cannot be told.
newest_version() {
  local url
  url=$(curl -fsSI -o /dev/null -w '%{redirect_url}' "https://github.com/$REPOSITORY/releases/latest" 2>/dev/null || true)
  case "$url" in */tag/v[0-9]*) printf '%s\n' "${url##*/tag/v}" ;; esac
}

server_address() {
  local address
  address=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit }}' || true)
  [ -n "$address" ] || address=$( { hostname -I 2>/dev/null | awk '{print $1}'; } || true)
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

# Whether somebody can be asked: the terminal has to open, not only be there.
# Without one (ssh without -t, cloud-init, a pipeline) /dev/tty exists and
# passes the permission tests, and opening it fails.
can_ask() { [ -r /dev/tty ] && [ -w /dev/tty ] && { true <> /dev/tty; } 2>/dev/null; }

set_env() { # set_env <name> <value>: replaces the line, or adds it
  if grep -q "^$1=" "$DIR/.env"; then
    sed -i "s|^$1=.*|$1=$2|" "$DIR/.env"
  else
    printf '%s=%s\n' "$1" "$2" >> "$DIR/.env"
  fi
}

compose() { (cd "$DIR" && docker compose "$@"); }

# What kind of failure Docker's own message names when an image cannot be downloaded.
pull_reason() { # pull_reason <docker's message>
  case "$1" in
    *"no space left"*) t "디스크 공간이 모자라요." "The disk is full." ;;
    *denied*|*unauthorized*|*forbidden*) t "레지스트리가 거절했어요 (권한 없음)." "The registry refused (access denied)." ;;
    *"manifest unknown"*|*"not found"*) t "레지스트리에 그 버전의 이미지가 없어요." "The registry has no image of that version." ;;
    *"no such host"*|*"name resolution"*|*"server misbehaving"*) t "레지스트리의 이름을 찾지 못했어요 (DNS)." "The registry's name could not be resolved (DNS)." ;;
    *timeout*|*"timed out"*|*"deadline exceeded"*) t "레지스트리가 시간 안에 답하지 않았어요." "The registry did not answer in time." ;;
    *"connection refused"*|*"network is unreachable"*|*"no route to host"*|*"connection reset"*) t "레지스트리에 연결하지 못했어요 (네트워크)." "Could not connect to the registry (network)." ;;
    *) t "이유는 위 Docker 메시지에 있어요." "The reason is in Docker's message above." ;;
  esac
}

# The server is up when its container runs the expected release and passes the
# image's own health check (the same wait as the multitap command, explained
# there): the release is the container's image version label, and "healthy"
# means the server inside answered the image's HEALTHCHECK with that version.
# Nothing is asked over the network, so the addresses in use make no
# difference. A container that keeps restarting is a failure at once.
container_line() {
  local id
  id=$(compose ps -aq broker 2>/dev/null | head -n 1 || true)
  [ -n "$id" ] || return 0
  docker inspect -f '{{.State.Status}} {{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}} {{.RestartCount}} {{index .Config.Labels "org.opencontainers.image.version"}}' "$id" 2>/dev/null || true
}
wait_ready() { # wait_ready <release>
  local i state health restarts release
  for i in $(seq 1 90); do
    state='' health='' restarts='' release=''
    read -r state health restarts release <<< "$(container_line)" || true
    if [ "$state" = running ] && [ "$health" = healthy ] && [ "$release" = "$1" ]; then return 0; fi
    [ "$health" != unhealthy ] || return 1
    if [[ $restarts =~ ^[0-9]+$ ]] && [ "$restarts" -ge 2 ]; then return 1; fi
    sleep 2
  done
  return 1
}
# The addresses the server itself has (src/access-cli.mjs in a one-off
# container of the service, as `multitap address` reads them). A release
# without that command gives no answer.
kv() { sed -n "s/^$1=//p" <<< "$2" | head -n 1; }
access_show() { compose run --rm --no-deps -T --entrypoint node broker /app/src/access-cli.mjs show 2>/dev/null < /dev/null || true; }
setup_code() {
  # No code in the log (setup already done) is an answer, not a failure under pipefail.
  { compose logs --no-color broker 2>/dev/null | grep -o '"setupCode":"[^"]*"' | tail -n 1 | sed 's/.*:"\(.*\)"/\1/'; } || true
}

main() {
  DIR=${MULTITAP_DIR:-/opt/multitap}
  check_system
  start_log
  printf '\n%s▶ %s%s\n' "$B" "$(t 'Multitap 서버 설치' 'Installing the Multitap server')" "$OFF"
  step 1 "$(t "시스템 확인" "Checking the system")"
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
  check_ct

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
    [ -n "$VERSION" ] || fail "$(t "새 버전을 확인하지 못했어요 (github.com). 인터넷 연결을 확인하거나 MULTITAP_VERSION=1.0.0 처럼 버전을 정해 주세요." "Could not find the newest release (github.com). Check the internet connection, or choose a version, e.g. MULTITAP_VERSION=1.0.0.")"
  fi
  case "$VERSION" in [0-9]*.[0-9]*.[0-9]*) ;; *) fail "$(t "버전을 알 수 없어요: $VERSION" "Unknown version: $VERSION")" ;; esac

  install -d -m 0750 "$DIR" "$DIR/backups"
  chmod 0700 "$DIR/backups"
  fetch compose.yaml "$DIR/compose.yaml"
  fetch .env.example "$DIR/.env.example"
  fetch deploy/docker/multitap /usr/local/bin/multitap
  chmod 0755 /usr/local/bin/multitap
  if [ "$KEEP_ENV" = yes ]; then
    ADDRESS=$( { grep -E '^MULTITAP_SERVER_ADDRESS=' "$DIR/.env" | tail -n 1 | cut -d= -f2-; } || true)
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
  # until an address is set on the console's Server access page. These .env
  # lines only start a server that has saved no addresses yet; what is saved
  # afterwards is what counts.
  LAN_URL='' PUBLIC_URL='' FOLLOW_PUBLIC=no
  if [ -n "$ADDRESS" ] && ! grep -qE '^MULTITAP_(HTTP|PUBLIC)_ORIGIN=.+' "$DIR/.env"; then
    local lan=${MULTITAP_LAN_HTTP:-ask} answer=''
    if [ "$lan" = ask ]; then
      if can_ask; then
        pf '\n  %s?%s %s앱과 Host가 이 주소로 접속하게 할까요?%s  %shttp://%s:18480%s\n' \
           '\n  %s?%s %sLet apps and Hosts connect at this address?%s  %shttp://%s:18480%s\n' "$YELLOW" "$OFF" "$B" "$OFF" "$CYAN" "$ADDRESS" "$OFF" > /dev/tty
        pf '    %s같은 네트워크용이에요. 암호화되지 않아요. 나중에 바꾸려면 관리자 페이지 > 서버 접속 설정%s\n    [Y/n]: ' \
           '    %sFor the same network. Not encrypted. To change it later: admin console > Server access%s\n    [Y/n]: ' "$DIM" "$OFF" > /dev/tty
        read -r answer < /dev/tty || answer=''
      fi
      case "$answer" in n|N|no|No) lan=0 ;; *) lan=1 ;; esac
    fi
    if [ "$lan" = 1 ]; then
      set_env MULTITAP_HTTP_ORIGIN "http://$ADDRESS:18480"
      set_env MULTITAP_ALLOW_INSECURE_HTTP 1
      set_env MULTITAP_PUBLISH_PORT "0.0.0.0:18480"
      LAN_URL="http://$ADDRESS:18480"
      ok "$(t "앱·Host 접속 주소: $LAN_URL" "Address for apps and Hosts: $LAN_URL")"
      # From outside over plain HTTP: only when someone says yes. Nothing that
      # is not encrypted is opened to the internet by pressing Enter, or
      # without a terminal; MULTITAP_PUBLIC_HTTP=1 opens it without asking.
      local public='' outside=${MULTITAP_PUBLIC_HTTP:-ask} reply=''
      public=$( { curl -fs --max-time 5 https://1.1.1.1/cdn-cgi/trace 2>/dev/null | sed -n 's/^ip=//p'; } || true)
      [[ $public =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || public=''
      if [ -n "$public" ] && [ "$public" != "$ADDRESS" ] && [ "$outside" != 0 ]; then
        if [ "$outside" = ask ] && can_ask; then
          pf '\n  %s?%s %s밖에서도 HTTP로 접속하게 할까요? (공유기 포트 포워딩)%s  %shttp://%s:18480%s\n' \
             '\n  %s?%s %sAlso allow HTTP connections from outside? (router port forwarding)%s  %shttp://%s:18480%s\n' "$YELLOW" "$OFF" "$B" "$OFF" "$CYAN" "$public" "$OFF" > /dev/tty
          pf '    %s%s인터넷을 지날 때 암호화되지 않아요. 밖에서는 HTTPS 주소를 권해요.%s\n' \
             '    %s%sNot encrypted over the internet. From outside, an HTTPS address is recommended.%s\n' "$YELLOW" "$B" "$OFF" > /dev/tty
          pf '    %s나중에 관리자 페이지 > 서버 접속 설정에서 켤 수 있어요.%s\n    [y/N]: ' \
             '    %sYou can turn it on later in the admin console > Server access.%s\n    [y/N]: ' "$DIM" "$OFF" > /dev/tty
          read -r reply < /dev/tty || reply=''
          case "$reply" in y|Y|yes|Yes|YES) outside=1 ;; *) outside=0 ;; esac
        elif [ "$outside" = ask ]; then
          outside=0
        fi
        if [ "$outside" = 1 ]; then
          # Listed from the first start. Once the server answers, following is
          # turned on for this address (below): no address changes then, so
          # nothing restarts and the setup code stays valid.
          set_env MULTITAP_HTTP_ORIGIN "http://$ADDRESS:18480,http://$public:18480"
          PUBLIC_URL="http://$public:18480"
          FOLLOW_PUBLIC=yes
          ok "$(t "밖에서 접속할 주소: $PUBLIC_URL (공인 IP가 바뀌면 서버가 따라가요)" "Address from outside: $PUBLIC_URL (the server follows the public IP when it changes)")"
          warn "$(t "인터넷을 지날 때 암호화되지 않아요. 밖에서 쓰려면 HTTPS 주소를 권해요." "Not encrypted over the internet. For use from outside, an HTTPS address is recommended.")"
        else
          note "$(t "밖에서 HTTP 접속($public)은 열지 않았어요. 열려면 관리자 페이지 > 서버 접속 설정" "HTTP from outside ($public) is not opened. To open it: admin console > Server access")"
        fi
      fi
    else
      note "$(t "앱·Host 접속 주소는 나중에 정해요: 관리자 페이지 > 서버 접속 설정" "Set the address for apps and Hosts later: admin console > Server access")"
    fi
  fi

  local image
  image=$( { grep -E '^MULTITAP_IMAGE=' "$DIR/.env" | cut -d= -f2-; } || true)
  image=${image:-$IMAGE_DEFAULT}
  if docker image inspect "$image:$VERSION" >/dev/null 2>&1; then
    ok "$(t "서버 이미지($VERSION)가 이미 있어요" "Server image $VERSION is already here")"
  elif [ -n "${MULTITAP_LOCAL:-}" ]; then
    # A saved image beside the files (docker save | gzip): installs without the internet registry.
    local saved
    saved=$( { ls "$MULTITAP_LOCAL"/multitap-broker-"$VERSION".tar* 2>/dev/null | head -n 1; } || true)
    [ -n "$saved" ] || fail "$(t "$MULTITAP_LOCAL 에 multitap-broker-$VERSION.tar.gz 이미지 파일이 없어요." "No image file multitap-broker-$VERSION.tar.gz in $MULTITAP_LOCAL.")"
    say "$(t "서버 이미지를 불러와요." "Loading the server image.")"
    logged docker load -q -i "$saved" || fail "$(t "이미지 파일을 불러오지 못했어요: $saved" "Could not load the image file: $saved")"
    docker image inspect "$image:$VERSION" >/dev/null 2>&1 || fail "$(t "불러온 파일에 $image:$VERSION 이미지가 없어요." "The loaded file has no $image:$VERSION image.")"
    ok "$(t "서버 이미지($VERSION)를 불러왔어요" "Server image $VERSION loaded")"
  else
    say "$(t "서버 이미지를 받아요." "Downloading the server image.")"
    # Docker's own words say why a download fails: they are shown, not guessed at.
    local pulled
    if ! pulled=$(compose pull --quiet 2>&1); then
      printf '%s\n' "$pulled" | tail -n 6 | sed 's/^/    /' >&2
      fail "$(t "서버 이미지($image:$VERSION)를 받지 못했어요. $(pull_reason "$pulled")" "Could not download the server image $image:$VERSION. $(pull_reason "$pulled")")"
    fi
    ok "$(t "서버 이미지($VERSION)를 받았어요" "Server image $VERSION downloaded")"
  fi

  step 4 "$(t "서버 시작" "Starting the server")"
  # The backstop for a CT without nesting or keyctl: no container starts at all.
  local tried
  if ! tried=$(docker run --rm --network none --entrypoint true "$image:$VERSION" 2>&1); then
    printf '%s\n' "$tried" | tail -n 4 | sed 's/^/    /' >&2
    if [ "$IN_CT" = yes ]; then
      fail "$(t "이 CT에서 Docker 컨테이너를 실행하지 못했어요. Proxmox에서 CT > 옵션 > 기능의 nesting(권한 없는 CT는 keyctl도)을 켜고 CT를 다시 시작한 뒤 이 명령을 다시 실행해 주세요." "Docker could not run a container in this CT. In Proxmox, turn on nesting under CT > Options > Features (and keyctl for an unprivileged CT), restart the CT, then run this command again.")"
    fi
    fail "$(t "Docker 컨테이너를 실행하지 못했어요. 'docker run hello-world'로 Docker가 되는지 확인해 주세요." "Docker could not run a container. Check Docker with 'docker run hello-world'.")"
  fi
  local started
  started=$(compose up -d --quiet-pull 2>&1) || { printf '%s\n' "$started" >&2; fail "$(t "서버를 시작하지 못했어요." "Could not start the server.")"; }
  say "$(t "서버가 응답하기를 기다려요." "Waiting for the server to answer.")"
  if ! wait_ready "$VERSION"; then
    # The server's own log, without the line that carries the setup code.
    { compose logs --no-color --tail 40 broker 2>&1 | grep -v 'setupCode' >&2; } || true
    fail "$(t "서버가 3분 안에 응답하지 않았어요. 위 기록을 확인해 주세요. 다시 보려면: multitap logs" "The server did not answer within 3 minutes. Check the log above. To see it again: multitap logs")"
  fi
  ok "$(t "서버($VERSION)가 응답해요" "Server $VERSION is answering")"

  touch "$DIR/.installed"
  # The same command an administrator would run: the server follows its public IP itself.
  if [ "$FOLLOW_PUBLIC" = yes ]; then
    /usr/local/bin/multitap public-ip on >/dev/null 2>&1 || warn "$(t "공인 IP 따라가기를 켜지 못했어요. 나중에: 관리자 페이지 > 서버 접속 설정" "Could not turn on public IP following. Later: admin console > Server access")"
  fi
  # The addresses as the server has them now. A release without the command
  # has them in its .env lines.
  local shown open app outside_url here
  here=${ADDRESS:-$(t '<이 서버 IP>' "<this server's IP>")}
  shown=$(access_show)
  if [ "$(kv ok "$shown")" = 1 ]; then
    app=$(kv primary "$shown") outside_url=$(kv followed "$shown")
    open=${app:-http://$here:$(kv publishedPort "$shown")}
  else
    app=$( { grep -E '^MULTITAP_PUBLIC_ORIGIN=' "$DIR/.env" | tail -n 1 | cut -d= -f2-; } || true)
    [ -n "$app" ] || app=$( { grep -E '^MULTITAP_HTTP_ORIGIN=' "$DIR/.env" | tail -n 1 | cut -d= -f2- | cut -d, -f1; } || true)
    open=${app:-http://$here:18480} outside_url=$PUBLIC_URL
  fi
  local code line
  code=$(setup_code)
  line='━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━'
  # The closing lines carry the setup code: they are shown, not recorded.
  [ "$LOG" = /dev/null ] || printf 'Installed: Multitap server %s. The closing lines (address, setup code) are not recorded here; multitap code shows the code again.\n' "$VERSION" >> "$LOG"
  stop_log
  printf '\n%s%s%s\n' "$GREEN" "$line" "$OFF"
  pf ' %s✔ 설치를 마쳤어요%s  (Multitap 서버 %s)\n' \
     ' %s✔ Installed%s  (Multitap server %s)\n' "$GREEN" "$OFF" "$VERSION"
  printf '%s%s%s\n\n' "$GREEN" "$line" "$OFF"
  pf '  %s①%s 브라우저로 열기   %s%s%s\n' \
     '  %s①%s Open in a browser   %s%s%s\n' "$CYAN" "$OFF" "$B" "$open" "$OFF"
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
  if [ -n "$app" ]; then
    pf '  %s→%s 앱과 Host에는 %s%s%s 를 넣어요.\n' \
       '  %s→%s In apps and Hosts, enter %s%s%s\n' "$CYAN" "$OFF" "$B" "$app" "$OFF"
    [ -z "$outside_url" ] || pf '  %s→%s 밖에서는 %s%s%s\n' \
       '  %s→%s From outside: %s%s%s\n' "$CYAN" "$OFF" "$B" "$outside_url" "$OFF"
  else
    pf '  %s→%s 앱과 Host가 붙기 전에: 관리자 페이지 > %s서버 접속 설정%s에서 주소를 정해요.\n' \
       '  %s→%s Before apps and Hosts can connect: set an address in the admin console > %sServer access%s.\n' "$CYAN" "$OFF" "$B" "$OFF"
  fi
  # From outside, in three lines.
  pf '\n  %s밖에서 접속하려면%s\n' '\n  %sTo connect from outside%s\n' "$B" "$OFF"
  local target=${ADDRESS:-$(t '이 서버' 'this server')}
  if [ -n "$outside_url" ]; then
    pf '  %s✔%s %s 를 등록했고, 공인 IP가 바뀌면 서버가 알아서 따라가요.\n' \
       '  %s✔%s %s is registered, and the server follows the public IP when it changes.\n' "$GREEN" "$OFF" "$outside_url"
    pf '  %s1%s 공유기 포트 포워딩: TCP 18480, UDP 18481 → %s\n' \
       '  %s1%s Router port forwarding: TCP 18480, UDP 18481 → %s\n' "$CYAN" "$OFF" "$target"
    pf '    %s포워딩을 안 하면 밖에서는 안 들어와요. 같은 네트워크는 그대로 돼요.%s\n' \
       '    %sWithout forwarding nothing comes in from outside. The same network keeps working.%s\n' "$DIM" "$OFF"
    pf '    %sHTTP는 암호화되지 않아요. 도메인이 있으면 HTTPS가 안전해요: 관리자 페이지 > 서버 접속 설정%s\n' \
       '    %sHTTP is not encrypted. With a domain, HTTPS is the safe way: admin console > Server access%s\n' "$DIM" "$OFF"
  else
    pf '  %s1%s 권장: 도메인과 HTTPS. 관리자 페이지 > 서버 접속 설정에 HTTPS 주소를 넣어요.\n' \
       '  %s1%s Recommended: a domain with HTTPS. Enter the HTTPS address in the admin console > Server access.\n' "$CYAN" "$OFF"
    pf '  %s2%s 간단: 공인 IP와 HTTP(암호화 안 됨). 같은 화면에서 ‘공인 IP 따라가기’를 켜요.\n' \
       '  %s2%s Simple: the public IP with HTTP (not encrypted). Turn on “Follow the public IP” on the same page.\n' "$CYAN" "$OFF"
    pf '    %s어느 쪽이든 공유기 포트 포워딩이 필요해요: UDP 18481, HTTP라면 TCP 18480도 → %s%s\n' \
       '    %sEither way the router must forward ports: UDP 18481, and TCP 18480 for HTTP → %s%s\n' "$DIM" "$target" "$OFF"
  fi
  pf '\n  %s⚠%s 방화벽이 있으면 18480(TCP)과 18481(UDP, 화면 전송)을 열어 주세요.\n' \
     '\n  %s⚠%s If there is a firewall, open 18480 (TCP) and 18481 (UDP, screen streaming).\n' "$YELLOW" "$OFF"
  pf '  %s관리 명령: multitap status · logs · update · backup · restore · code · address · public-ip · uninstall%s\n\n' \
     '  %sCommands: multitap status · logs · update · backup · restore · code · address · public-ip · uninstall%s\n\n' "$DIM" "$OFF"
}
main "$@"
