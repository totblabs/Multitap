# The fake `docker` of docker-restore-command.test.mjs (see there).
set -euo pipefail
F=$FAKE_ROOT
printf '%s\n' "$*" >> "$F/calls"
case "$1" in
  compose)
    shift
    case "$1" in
      stop) echo stopped > "$F/state" ;;
      up|start|restart) echo running > "$F/state" ;;
      ps) if [ "$(cat "$F/state")" = running ] || [[ " $* " != *" --status running "* ]]; then echo cid; fi ;;
    esac ;;
  image) exit 0 ;;
  # A server that cannot start keeps restarting: the restart count says so at once.
  inspect) if [ -e "$F/volume/poison" ]; then echo 2; else echo 0; fi ;;
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
      # tar czf <out> -C /data . (save), or tar xzf - -C /data ... (restore, as the server's account)
      tar) if [ "$1" = xzf ]; then tar xzf - -C "$(map "$4")" --no-same-owner --no-same-permissions; else tar czf "$(map "$2")" -C "$(map "$4")" .; fi ;;
      # sh -c "find /data -mindepth 1 -delete && chown ...": empties the volume (owners are not tested here)
      sh) d=$(map /data); find "$d" -mindepth 1 -delete ;;
      test) [ -f "$(map "$2")" ] ;;
      node)
        args=()
        for a in "$@"; do case "$a" in /*) args+=("$(map "$a")") ;; *) args+=("$a") ;; esac; done
        if [ -n "${FAKE_FAIL_APPLY:-}" ] && [ "${args[1]}" = apply ]; then printf 'ok=0\ncode=restore_staging_changed\n'; exit 2; fi
        exec "$NODE" "${args[@]}" ;;
    esac ;;
esac
