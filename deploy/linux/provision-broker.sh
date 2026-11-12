#!/bin/bash
set -euo pipefail
umask 077
archive=${1:?archive required}
expected_sha=${2:?SHA-256 required}
release=${3:?release required}
reset_mode=${4:-keep}
public_origin=${5:-${MULTITAP_PUBLIC_ORIGIN:-}}
[[ $reset_mode == keep || $reset_mode == reset-initial ]] || exit 1
root=/opt/multitap
runtime_version=24.22.0

[[ $(id -u) == 0 ]] || { echo 'Root is required for service installation.' >&2; exit 1; }
[[ $(uname -m) == x86_64 ]] || { echo 'This package targets Linux x86_64.' >&2; exit 1; }
[[ $archive == /opt/multitap-stage/*/server.tar.gz && $expected_sha =~ ^[a-f0-9]{64}$ && $release =~ ^[0-9A-Za-z._-]+$ ]] || exit 1
for command in curl tar xz sha256sum systemctl runuser ss mktemp stat df awk; do command -v "$command" >/dev/null; done
if [[ -e $root && ! -f $root/.multitap-managed ]]; then echo 'Unrecognized deployment directory; refusing to overwrite.' >&2; exit 1; fi
if [[ -f /etc/systemd/system/multitap-broker.service ]] && ! grep -q '# Multitap managed broker' /etc/systemd/system/multitap-broker.service; then echo 'Unrecognized service file.' >&2; exit 1; fi
# Deployment free-space guard: run before writes or stopping the live service.
for space_path in /opt /var/lib; do
  if [[ $space_path == /var/lib && -d /var/lib/multitap ]]; then space_path=/var/lib/multitap; fi
  available_kib=$(df -Pk -- "$space_path" | awk 'NR==2 {print $4}')
  if [[ ! $available_kib =~ ^[0-9]+$ || $available_kib -lt 262144 ]]; then
    echo 'At least 256 MiB of available disk space is required before deployment; the running broker was not stopped.' >&2
    exit 1
  fi
done
# End deployment free-space guard.
printf '%s  %s\n' "$expected_sha" "$archive" | sha256sum -c -
install -d -m 755 "$root" "$root/releases" "$root/runtime"
touch "$root/.multitap-managed"
if ! id multitap >/dev/null 2>&1; then useradd --system --home-dir /var/lib/multitap --shell /usr/sbin/nologin multitap; fi
install -d -o multitap -g multitap -m 700 /var/lib/multitap
install -d -o root -g root -m 750 /etc/multitap

runtime="$root/runtime/node-v${runtime_version}-linux-x64"
if [[ ! -x $runtime/bin/node ]]; then
  cache="$root/runtime/downloads"
  install -d -m 700 "$cache"
  name="node-v${runtime_version}-linux-x64.tar.xz"
  curl --fail --location --silent --show-error --retry 3 "https://nodejs.org/dist/v${runtime_version}/SHASUMS256.txt" -o "$cache/SHASUMS256.txt"
  curl --fail --location --silent --show-error --retry 3 "https://nodejs.org/dist/v${runtime_version}/$name" -o "$cache/$name"
  (cd "$cache"; grep "  $name\$" SHASUMS256.txt | sha256sum -c -)
  tar -xJf "$cache/$name" -C "$root/runtime"
  chmod -R a+rX "$runtime"
fi
[[ $($runtime/bin/node --version) == v${runtime_version} ]] || exit 1
release_dir="$root/releases/$release"
[[ ! -e $release_dir ]] || { echo 'Release already exists.' >&2; exit 1; }
install -d -m 755 "$release_dir"
tar -xzf "$archive" -C "$release_dir" --no-same-owner
chmod -R a+rX,go-w "$release_dir"
# Verify the packaged, pure-JavaScript relay dependency before stopping this broker.
(cd "$release_dir"; "$runtime/bin/node" --input-type=module -e 'import { WebSocketServer } from "ws"; if (typeof WebSocketServer !== "function") process.exit(1);')
previous=$(readlink -f "$root/current" || true)
if [[ -e $root/current && ! -L $root/current ]]; then echo 'Current release must be a managed symlink.' >&2; exit 1; fi
# Validate existing settings as data and preserve them verbatim. New installs
# explicitly opt in to HTTP/TLS through environment options; never infer either
# from a successful backend probe, a forwarded header or a failed HTTPS attempt.
health_env_file=/etc/multitap/broker.env
transport_json=$("$runtime/bin/node" --input-type=module - "$release_dir" "$health_env_file" "$public_origin" <<'NODE'
import { pathToFileURL } from 'node:url';
import { lstat } from 'node:fs/promises';
const [release, file, origin] = process.argv.slice(2);
const { readDeploymentEnvironment, localHealthOptions, deploymentEnvironment } = await import(pathToFileURL(release + '/update/deployment-health.mjs'));
const { deploymentTransport } = await import(pathToFileURL(release + '/update/transport-policy.mjs'));
let config;
try { await lstat(file); config = await readDeploymentEnvironment(file); }
catch (error) {
  if (error.code !== 'ENOENT') throw Error('update_installation_invalid');
  const names = ['MULTITAP_PUBLIC_ORIGIN','MULTITAP_ALLOW_INSECURE_HTTP','MULTITAP_HOST','MULTITAP_PORT','MULTITAP_TLS_MODE','MULTITAP_TLS_CERT_FILE','MULTITAP_TLS_KEY_FILE','MULTITAP_TRUSTED_PROXIES'];
  const input = { ...process.env, MULTITAP_PUBLIC_ORIGIN: origin, MULTITAP_HOST: process.env.MULTITAP_HOST ?? '0.0.0.0' };
  const rows = names.filter(name => input[name] !== undefined).map(name => { if (typeof input[name] !== 'string' || /[\r\n\0]/.test(input[name])) throw Error('update_installation_invalid'); return name + '=' + input[name]; });
  config = deploymentEnvironment(rows.join('\n'));
}
const { options } = localHealthOptions(config), localHost = options.hostname.includes(':') ? '[' + options.hostname + ']' : options.hostname;
process.stdout.write(JSON.stringify({ ...config, healthUrl: 'http://' + localHost + ':' + config.port + '/healthz' }));
NODE
)
listen_port=$("$runtime/bin/node" -e 'process.stdout.write(String(JSON.parse(process.argv[1]).port))' "$transport_json")
health_url=$("$runtime/bin/node" -e 'process.stdout.write(JSON.parse(process.argv[1]).healthUrl)' "$transport_json")
health_tls_mode=$("$runtime/bin/node" -e 'process.stdout.write(JSON.parse(process.argv[1]).tlsMode)' "$transport_json")
if ! systemctl is-active --quiet multitap-broker && [[ -n $(ss -H -lnt "sport = :$listen_port") ]]; then echo "Port $listen_port is already in use." >&2; exit 1; fi
# Guard rollback before stopping the broker, including failures while backing
# up. A partial or unvalidated backup must never replace the live database.
broker_healthy() {
  local response='' attempt
  for attempt in 1 2 3 4 5; do
    if systemctl is-active --quiet multitap-broker &&
       response=$(if [[ $health_tls_mode == direct ]]; then "$runtime/bin/node" "$release_dir/update/deployment-health.mjs" "$health_env_file"; else curl --fail --silent --max-time 2 "$health_url"; fi) &&
       [[ $response == *'"service":"multitap-broker"'* && $response == *'"status":"ok"'* ]]; then return 0; fi
    sleep 1
  done
  return 1
}
backup_file="/var/lib/multitap/backups/broker-$release.sqlite"
backup_ready=false
backup_temp=''
account_backup_ready=false
account_backup_temp=''
# /var/lib/multitap belongs to the broker's account. Root never follows a link
# that account may have planted there: its files are checked after the broker
# is stopped (a plain file of that account with one name) and copied, made and
# renamed as that account, so a link reaches only what it could read anyway.
as_broker() { runuser -u multitap -- "$@"; }
broker_file() {
  [[ -f $1 && ! -L $1 ]] && [[ $(stat -c '%U:%h' -- "$1") == multitap:1 ]]
}
broker_backups() {
  local dir=/var/lib/multitap/backups
  if [[ ! -e $dir && ! -L $dir ]]; then as_broker mkdir -m 700 -- "$dir"; fi
  if [[ ! -d $dir || -L $dir || $(stat -c '%U' -- "$dir") != multitap ]]; then
    echo 'The broker backup directory is not a plain directory of the broker account.' >&2; return 1
  fi
  as_broker chmod 700 -- "$dir"
}
updater_unit_stage=${archive%/server.tar.gz}
updater_unit_migration() {
  "$runtime/bin/node" --input-type=module - "$release_dir" "$updater_unit_stage" "$1" <<'NODE'
import { pathToFileURL } from 'node:url';
const [release, stage, action] = process.argv.slice(2);
const { prepareUpdaterUnitMigration, applyUpdaterUnitMigration } = await import(pathToFileURL(release + '/update/systemd.mjs'));
if (action === 'prepare') await prepareUpdaterUnitMigration(stage);
else if (action === 'apply' || action === 'restore') await applyUpdaterUnitMigration(stage, { restore: action === 'restore' });
else throw Error('update_updater_unit_invalid');
NODE
}
# Immutable exact-byte snapshot before stopping the live broker or mutating its
# database. An absent updater remains absent; no service is created here.
updater_unit_migration prepare
rollback() {
  trap - ERR
  local stopped=false restored=false restarted=false updater_unit_restored=true
  if systemctl stop multitap-broker; then stopped=true; fi
  if [[ $backup_ready == true ]]; then
    if [[ $stopped == true ]]; then
      # Stage restoration as a private file before replacing the migrated DB.
      local restore_temp=''
      if broker_file "$backup_file" && restore_temp=$(as_broker mktemp /var/lib/multitap/broker.sqlite.restore.XXXXXX) && as_broker cp -- "$backup_file" "$restore_temp" && as_broker chmod 600 -- "$restore_temp" && as_broker mv -fT -- "$restore_temp" /var/lib/multitap/broker.sqlite && as_broker rm -f -- /var/lib/multitap/broker.sqlite-wal /var/lib/multitap/broker.sqlite-shm; then
        restored=true
        if [[ $account_backup_ready == true ]]; then
          local account_restore_temp=''
          if ! { broker_file "$backup_file.initial-account.json" && account_restore_temp=$(as_broker mktemp /var/lib/multitap/initial-account.json.restore.XXXXXX) && as_broker cp -- "$backup_file.initial-account.json" "$account_restore_temp" && as_broker chmod 600 -- "$account_restore_temp" && as_broker mv -fT -- "$account_restore_temp" /var/lib/multitap/initial-account.json; }; then
            restored=false
            echo 'Could not restore the prior private initial-account metadata.' >&2
          fi
        fi
      else
        echo 'Could not restore the validated database backup; the previous broker was not started.' >&2
      fi
    else
      echo 'Could not stop the broker for database restoration; the database was not replaced.' >&2
    fi
  else
    # Backup failure happened before bootstrap/migration. Keep the original DB,
    # WAL and account files intact and restart its original application.
    restored=true
  fi
  if [[ -n $backup_temp ]]; then rm -f -- "$backup_temp" || true; fi
  if [[ -n $account_backup_temp ]]; then rm -f -- "$account_backup_temp" || true; fi
  if ! updater_unit_migration restore; then
    updater_unit_restored=false
    echo 'Could not verify restoration of the managed updater unit; its immutable backup was retained.' >&2
  fi
  if [[ $restored == true && $previous == "$root/releases/"* && -d $previous ]]; then
    if ln -sfn "$previous" "$root/current" && systemctl restart multitap-broker && broker_healthy; then restarted=true; fi
  fi
  if [[ $restarted == true && $updater_unit_restored == true && $backup_ready == true ]]; then
    echo 'Deployment failed; restored the previous broker release and validated database backup.' >&2
  elif [[ $restarted == true && $updater_unit_restored == true ]]; then
    echo 'Deployment failed before migration; kept the original database and restarted the previous broker.' >&2
  else
    echo 'Deployment failed; automatic recovery is incomplete. Check this broker service and its validated backup.' >&2
  fi
  exit 1
}
trap rollback ERR
# Snapshot only this broker's database before applying a schema migration.
printf '%s\n' 'MULTITAP_UPDATE_PHASE backing_up'
if [[ -e /var/lib/multitap/broker.sqlite || -L /var/lib/multitap/broker.sqlite ]]; then
  systemctl stop multitap-broker
  broker_file /var/lib/multitap/broker.sqlite || { echo 'The broker database is not a plain file of the broker account.' >&2; false; }
  broker_backups
  [[ ! -e $backup_file && ! -L $backup_file ]] || { echo 'A database backup already exists for this release.' >&2; false; }
  backup_temp=$(as_broker mktemp "$backup_file.tmp.XXXXXX")
  as_broker "$runtime/bin/node" --input-type=module - "$backup_temp" <<'NODE'
import { DatabaseSync, backup } from 'node:sqlite';
const source = new DatabaseSync('/var/lib/multitap/broker.sqlite', { readOnly: true });
try { await backup(source, process.argv[2]); } finally { source.close(); }
const copy = new DatabaseSync(process.argv[2], { readOnly: true });
try {
  const result = copy.prepare('PRAGMA quick_check').all();
  if (result.length !== 1 || Object.values(result[0])[0] !== 'ok') throw new Error('database_backup_integrity_failed');
} finally { copy.close(); }
NODE
  as_broker chmod 600 -- "$backup_temp"
  as_broker mv -fT -- "$backup_temp" "$backup_file"
  backup_temp=''
  backup_ready=true
  if [[ -e /var/lib/multitap/initial-account.json || -L /var/lib/multitap/initial-account.json ]]; then
    broker_file /var/lib/multitap/initial-account.json || { echo 'The initial account file is not a plain file of the broker account.' >&2; false; }
    account_backup_temp=$(as_broker mktemp "$backup_file.initial-account.json.tmp.XXXXXX")
    as_broker cp -- /var/lib/multitap/initial-account.json "$account_backup_temp"
    as_broker chmod 600 -- "$account_backup_temp"
    as_broker mv -fT -- "$account_backup_temp" "$backup_file.initial-account.json"
    account_backup_temp=''
    account_backup_ready=true
  fi
fi
printf '%s\n' 'MULTITAP_UPDATE_PHASE applying'
ln -sfn "$release_dir" "$root/current"
if [[ ! -f /etc/multitap/broker.env ]]; then
  [[ ! -e /etc/multitap/broker.env && ! -L /etc/multitap/broker.env ]] || { echo 'Invalid broker environment path.' >&2; false; }
  (set -o noclobber; "$runtime/bin/node" - "$transport_json" > /etc/multitap/broker.env <<'NODE'
const c = JSON.parse(process.argv[2]);
const env = { NODE_ENV: 'production', MULTITAP_HOST: c.host, MULTITAP_PORT: String(c.port), MULTITAP_DATA_DIR: '/var/lib/multitap', MULTITAP_ALLOW_INSECURE_HTTP: c.allowInsecureHttp ? '1' : '0', MULTITAP_TLS_MODE: c.tlsMode };
if (c.publicOrigin !== null) env.MULTITAP_PUBLIC_ORIGIN = c.publicOrigin;
if (c.tlsFiles) { env.MULTITAP_TLS_CERT_FILE = c.tlsFiles.certFile; env.MULTITAP_TLS_KEY_FILE = c.tlsFiles.keyFile; }
for (const [key, value] of Object.entries(env)) process.stdout.write(key + '=' + JSON.stringify(value) + '\n');
NODE
  )
  chmod 600 /etc/multitap/broker.env
fi
runuser -u multitap -- env MULTITAP_DATA_DIR=/var/lib/multitap "$runtime/bin/node" "$root/current/src/bootstrap.mjs" "$reset_mode"
cat > "$root/admin" <<ADMIN
#!/bin/sh
set -eu
exec runuser -u multitap -- env MULTITAP_DATA_DIR=/var/lib/multitap "$runtime/bin/node" "$root/current/src/admin.mjs" "\$@"
ADMIN
chmod 750 "$root/admin"
cat > /etc/systemd/system/multitap-broker.service <<SERVICE
# Multitap managed broker
[Unit]
Description=Multitap account and desktop broker
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=multitap
Group=multitap
WorkingDirectory=$root/current
EnvironmentFile=/etc/multitap/broker.env
Environment=MULTITAP_DEPLOYMENT_KIND=systemd
Environment=MULTITAP_UPDATE_SOCKET=/run/multitap-update/control.sock
ExecStart=$runtime/bin/node $root/current/src/index.mjs
Restart=on-failure
RestartSec=3
TimeoutStopSec=15
UMask=0077
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/multitap -/run/multitap-update
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6

[Install]
WantedBy=multi-user.target
SERVICE
systemctl daemon-reload
systemctl enable multitap-broker >/dev/null
printf '%s\n' 'MULTITAP_UPDATE_PHASE restarting'
systemctl restart multitap-broker
expected_version=$("$runtime/bin/node" -p 'JSON.parse(require("node:fs").readFileSync(process.argv[1],"utf8")).version' "$release_dir/package.json")
healthy=false
printf '%s\n' 'MULTITAP_UPDATE_PHASE health_check'
for attempt in $(seq 1 25); do
  if response=$("$runtime/bin/node" "$release_dir/update/deployment-health.mjs" "$health_env_file" "$expected_version") &&
     "$runtime/bin/node" -e 'try { const body=JSON.parse(process.argv[1]); process.exit(body.service==="multitap-broker"&&body.version===process.argv[2]&&body.status==="ok"?0:1); } catch { process.exit(1); }' "$response" "$expected_version"; then healthy=true; break; fi
  sleep 1
done
if [[ $healthy != true ]]; then
  rollback
fi
"$runtime/bin/node" "$release_dir/update/deployment-health.mjs" "$health_env_file" "$expected_version"
printf '\n'
systemctl is-active multitap-broker
# Reload only. The running updater finishes/persists this operation before its
# existing terminal self-refresh creates the new mount namespace.
updater_unit_migration apply
trap - ERR
printf '%s\n' 'MULTITAP_UPDATE_PHASE complete'
