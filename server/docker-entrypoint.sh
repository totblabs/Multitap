#!/bin/sh
set -eu
umask 077

# No origin means admin-only bootstrap; configuration()/strict runtime admission
# keeps enrollment, desktops and relay unavailable until access is configured.
: "${MULTITAP_DATA_DIR:=/var/lib/multitap}"
export MULTITAP_DATA_DIR
if [ ! -d "$MULTITAP_DATA_DIR" ] || [ ! -w "$MULTITAP_DATA_DIR" ]; then
  printf '%s\n' 'Multitap data volume must be writable by UID/GID 1000:1000.' >&2
  exit 1
fi

# Validate/merge immutable release inputs before bootstrap can touch accounts.
# Existing volume files/catalog entries are never overwritten by image seeding.
case "${MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE:-false}" in
  false) node /app/src/download-bundle.mjs seed /opt/multitap-release-bundle "$MULTITAP_DATA_DIR/downloads" ;;
  true) node /app/src/download-bundle.mjs seed /opt/multitap-release-bundle "$MULTITAP_DATA_DIR/downloads" --allow-empty ;;
  *) printf '%s\n' 'Invalid empty-bundle option.' >&2; exit 1 ;;
esac

# keep preserves existing accounts; an empty volume gets restricted admin/admin.
node /app/src/bootstrap.mjs keep

# The server runs as a child and is started again, in this same container,
# when it ends with exit code 75: that is how it applies a saved change of its
# own access settings (src/self-access.mjs). Any other end is the container's.
child=
trap 'if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null || true; fi' TERM INT
while :; do
  "$@" &
  child=$!
  code=0
  wait "$child" || code=$?
  # A forwarded signal interrupts wait: wait for the server's own exit.
  while kill -0 "$child" 2>/dev/null; do
    code=0
    wait "$child" || code=$?
  done
  [ "$code" -eq 75 ] || exit "$code"
done
