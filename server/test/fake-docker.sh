# The fake `docker` of the deploy/docker/multitap tests (docker-command-fixture.mjs).
# State lives in $FAKE_ROOT: `state` (running|stopped), `volume/` (the data
# volume), `container-version` and `container.env` (the release and .env the
# container was last created with), `images` (the local release tags), `calls`.
set -euo pipefail
F=$FAKE_ROOT
printf '%s\n' "$*" >> "$F/calls"
dotenv() { { sed -n "s/^$1=//p" "$MULTITAP_DIR/.env" | tail -n 1; } || true; }
# A path as this machine's node reads it (Git Bash hands node Windows paths).
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
# An image from before the server access command (FAKE_NO_ACCESS_CLI names its release).
has_access_cli() { [ "$(dotenv MULTITAP_VERSION)" != "${FAKE_NO_ACCESS_CLI:-none}" ]; }
case "$1" in
  --version) echo 'Docker version 27.0.0, build fake' ;;
  load) exit 0 ;;
  compose)
    shift
    case "$1" in
      # The plugin is there unless the test says Docker is not installed yet (install.sh then installs it).
      version) [ -z "${FAKE_NO_COMPOSE:-}" ] || [ -e "$F/compose-plugin" ] || exit 1 ;;
      pull) if [ -n "${FAKE_PULL_ERROR:-}" ]; then printf '%s
' "$FAKE_PULL_ERROR" >&2; exit 1; fi ;;
      stop|down) echo stopped > "$F/state" ;;
      start|restart) echo running > "$F/state" ;;
      # A container is created from the release and the settings of that moment.
      up) echo running > "$F/state"; dotenv MULTITAP_VERSION > "$F/container-version"; cp "$MULTITAP_DIR/.env" "$F/container.env"; cp "$MULTITAP_DIR/compose.yaml" "$F/container.compose.yaml" ;;
      ps) if [ "$(cat "$F/state")" = running ] || [[ " $* " != *" --status running "* ]]; then echo cid; fi ;;
      logs) if [ -f "$F/logs" ]; then cat "$F/logs"; fi ;;
      # compose run --rm --no-deps -T --entrypoint node broker /app/src/access-cli.mjs <arguments>:
      # this checkout's access-cli on the volume folder, with the environment compose.yaml gives the service.
      run)
        while [ $# -gt 0 ] && [ "$1" != /app/src/access-cli.mjs ]; do shift; done
        [ $# -gt 0 ] || exit 1
        shift
        has_access_cli || { echo "Cannot find module '/app/src/access-cli.mjs'" >&2; exit 1; }
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' exec "$NODE" "$(native "$FAKE_APP/test/fake-access-cli.mjs")" "$(native "$F/volume")" "$(native "$MULTITAP_DIR/.env")" "$@" ;;
    esac ;;
  image)
    case "$2" in
      # docker image ls --format '{{.Tag}}' <repository>
      ls) if [ -f "$F/images" ]; then cat "$F/images"; fi ;;
      rm) tag=${3##*:}; grep -qxF "$tag" "$F/images" || exit 1; grep -vxF "$tag" "$F/images" > "$F/images.new" || true; mv "$F/images.new" "$F/images" ;;
      inspect) [ -z "${FAKE_NO_IMAGE:-}" ] ;;
      *) exit 0 ;;
    esac ;;
  pull) exit 0 ;;
  info) printf '%s\n' "$F" ;;
  # docker inspect -f <format> <id>: state, health, restart count and the image's release label.
  # A server that cannot start keeps restarting: the restart count says so at once.
  inspect)
    release=$(cat "$F/container-version" 2>/dev/null || echo "$FAKE_VERSION")
    state=exited; [ "$(cat "$F/state")" != running ] || state=running
    broken=no
    [ ! -e "$F/volume/poison" ] || broken=yes
    [ "$release" != "${FAKE_BROKEN_RELEASE:-none}" ] || broken=yes
    if [ -n "${FAKE_BROKEN_POLICY:-}" ] && grep -qF "$FAKE_BROKEN_POLICY" "$F/volume/access/policy.json" 2>/dev/null; then broken=yes; fi
    if [ "$broken" = yes ]; then echo "$state starting 2 $release"; else echo "$state ${FAKE_HEALTH:-healthy} 0 $release"; fi ;;
  run)
    shift
    mounts=() entry=''
    while [ $# -gt 0 ]; do
      case "$1" in
        --rm|-i|--read-only) shift ;;
        --network|--user|--tmpfs|--cap-drop|--security-opt|--pids-limit|--memory) shift 2 ;;
        -v) mounts+=("$2"); shift 2 ;;
        --entrypoint) entry=$2; shift 2 ;;
        *) shift; break ;;
      esac
    done
    map() {
      local m spec host target
      for m in "${mounts[@]}"; do
        spec=${m%:ro}; target=${spec##*:}; host=${spec%:*}
        [ "$host" = multitap-broker-data ] && host=$F/volume
        case "$1" in "$target"|"$target"/*) printf '%s\n' "$host${1#"$target"}"; return ;; esac
      done
      case "$1" in /app/*) printf '%s\n' "$FAKE_APP${1#/app}"; return ;; esac
      printf '%s\n' "$1"
    }
    case "$entry" in
      # docker run --rm --network none --entrypoint true <image>: can this machine run a container at all
      true) if [ -n "${FAKE_RUN_ERROR:-}" ]; then printf '%s
' "$FAKE_RUN_ERROR" >&2; exit 125; fi ;;
      # tar czf /out/<file> [options] -C /data .  (save), or
      # tar xzf - -C /data [options]              (restore, as the server's account)
      tar) args=(); for a in "$@"; do case "$a" in /*) args+=("$(map "$a")") ;; *) args+=("$a") ;; esac; done; tar "${args[@]}" ;;
      # sh -c "find /data ... -delete && chown ...": empties the volume as the command says
      # (owners are not tested here).
      sh) d=$(map /data); script=${2%% && chown*}; (cd "$d" && eval "${script//\/data/.}") ;;
      test) case "$2" in /app/src/access-cli.mjs) has_access_cli ;; *) [ -f "$(map "$2")" ] ;; esac ;;
      node)
        args=()
        for a in "$@"; do case "$a" in /*) args+=("$(map "$a")") ;; *) args+=("$a") ;; esac; done
        if [ -n "${FAKE_FAIL_APPLY:-}" ] && [ "${args[1]}" = apply ]; then printf 'ok=0\ncode=restore_staging_changed\n'; exit 2; fi
        exec "$NODE" "${args[@]}" ;;
    esac ;;
esac
