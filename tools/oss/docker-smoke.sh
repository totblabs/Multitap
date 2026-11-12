#!/bin/sh
# Multitap broker Docker smoke test (CI-ready; needs Docker Engine with the
# compose plugin, curl and a POSIX shell; no Node on the machine).
#
#   sh tools/oss/docker-smoke.sh
#
# 1. builds image A from server/ with a download bundle (synthetic fixtures, or
#    the prepared bundle in MULTITAP_SMOKE_BUNDLE), and image B with a second,
#    newer bundle that does not list A's releases;
# 2. starts A through the public compose.yaml with its own project, ports and
#    data volume and no address yet: the first setup opens by the server
#    address (MULTITAP_SERVER_ADDRESS) from the root URL, only on setup paths;
# 3. sets an HTTPS address and, beside it, the LAN HTTP address
#    (MULTITAP_HTTP_ORIGIN) that every later check uses; waits for the container
#    health check and /healthz, checks the HTTP health profile the Windows
#    client's connection test reads, that /api/v1/downloads lists client, Host
#    and Android, /get/* resolve and a download matches its SHA-256;
# 4. switches the same volume to image B (the documented update path) and
#    checks every A file is still listed and byte-identical, B's are added,
#    the vault key is unchanged and the accounts were preserved;
# 5. restarts once more;
# 6. completes the first setup and changes the server access settings the way
#    the console does (the admin API): a save the server applies by itself
#    (same container, the server process started again, the policy in the data
#    volume), a save whose new main address cannot answer (the server goes back
#    to the previous settings on its own), and the same settings read and
#    written by access-cli.mjs, the command `multitap address` runs;
# 7. as root only: runs deploy/docker/multitap itself against this project
#    (status, address, an HTTPS-only policy and the wait that follows, backup);
# 8. removes everything it created (KEEP=1 keeps it).
#
# Environment: MULTITAP_SMOKE_BUNDLE (prepared bundle directory for image A),
# SMOKE_PORT (28480), SMOKE_UDP_PORT (28481), SMOKE_ADDRESS (this machine's IP
# when it cannot be found), KEEP (0).
# It never touches another compose project, volume or image.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
id=$(date +%s)-$$
project="multitap-smoke-$id"
volume="multitap-smoke-data-$id"
image="multitap-broker-smoke-$id"
port=${SMOKE_PORT:-28480}
udp_port=${SMOKE_UDP_PORT:-28481}
# This machine's own IP, not 127.0.0.1: a saved access change is checked by the
# server asking its new main address, and inside the container 127.0.0.1 is
# the container itself.
address=${SMOKE_ADDRESS:-$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n 1)}
[ -n "$address" ] || address=$(hostname -I 2>/dev/null | cut -d' ' -f1)
origin="http://$address:$port"
loopback="http://127.0.0.1:$port"
work=$(mktemp -d "${TMPDIR:-/tmp}/multitap-smoke.XXXXXX")
env_file="$work/smoke.env"
base=$(sed -n 's/^FROM \([^ ]*\).*/\1/p' "$repo/server/Dockerfile" | head -n 1)
version=$(sed -n 's/^  "version": "\([0-9][0-9.]*\)",$/\1/p' "$repo/server/package.json" | head -n 1)

say() { printf '%s\n' "smoke: $*"; }
fail() { printf '%s\n' "smoke: FAIL: $*" >&2; exit 1; }
compose() { docker compose -f "$repo/compose.yaml" -p "$project" --env-file "$env_file" "$@"; }
cleanup() {
  status=$?
  if [ "$status" -ne 0 ] && [ -f "$env_file" ]; then compose logs --no-color --tail=80 broker 2>/dev/null | grep -v initial_setup_code >&2 || true; fi
  if [ "${KEEP:-0}" = 1 ]; then say "kept project $project, volume $volume, images $image:a/$image:b, work $work"; return; fi
  [ -f "$env_file" ] && compose down -v --remove-orphans >/dev/null 2>&1 || true
  docker volume rm "$volume" >/dev/null 2>&1 || true
  docker image rm "$image:a" "$image:b" "$image:$version" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

[ -n "$base" ] && [ -n "$version" ] || fail "cannot read the base image or broker version"
[ -n "$address" ] || fail "cannot find this machine's IP address (set SMOKE_ADDRESS)"
command -v curl >/dev/null || fail "curl is required"
docker compose version >/dev/null || fail "the Docker compose plugin is required"
if docker volume inspect "$volume" >/dev/null 2>&1; then fail "volume $volume already exists"; fi
say "broker $version, base $base, project $project, address $origin"

# Node in the pinned base image runs the repository's own bundle tool.
node_run() { docker run --rm -u "$(id -u):$(id -g)" -v "$work:/work" -v "$repo/server/src:/app/src:ro" -w /work "$base" node "$@"; }

cat >"$work/make-bundle.mjs" <<'EOF'
// Synthetic, non-executable release fixtures with the receipts the release
// tools write, turned into a bundle by server/src/download-bundle.mjs itself.
import { mkdir, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { createDownloadBundle } from '/app/src/download-bundle.mjs';
const [name, version] = process.argv.slice(2), dir = '/work/artifacts-' + name;
const sha = b => createHash('sha256').update(b).digest('hex');
await mkdir(dir);
const entries = [];
for (const product of ['client', 'host']) {
  const filename = `Multitap${product === 'host' ? '-Host' : ''}-${version}-windows-x64.exe`, bytes = Buffer.from('Multitap smoke fixture ' + filename);
  const entry = { product, version, filename, size: bytes.length, sha256: sha(bytes), changelog: 'Smoke fixture ' + version, releasedAt: '2026-11-03T00:00:00.000Z' };
  const files = (product === 'host' ? ['bin/MultitapHostSettings.exe', 'multitap-host.json'] : ['bin/Multitap.exe', 'MultitapClientSetup.exe']).map(path => ({ path, size: 1, sha256: 'a'.repeat(64) }));
  await writeFile(`${dir}/${filename}`, bytes); await writeFile(`${dir}/${filename}.json`, JSON.stringify({ ...entry, payloadVerified: true, files }));
  entries.push(entry);
}
for (const abi of ['arm64-v8a', 'armeabi-v7a', 'x86_64']) {
  const filename = `Multitap-${version}-android-${abi}.apk`, bytes = Buffer.from('Multitap smoke fixture ' + filename);
  const entry = { product: 'android-client', version, abi, filename, format: 'apk', size: bytes.length, sha256: sha(bytes), changelog: 'Smoke fixture ' + version };
  await writeFile(`${dir}/${filename}`, bytes);
  await writeFile(`${dir}/${filename}.verification.json`, JSON.stringify({ policyVersion: 1, ...entry, format: undefined, signerSha256: 'c'.repeat(64), payloadVerified: true, signatureVerified: true, contentSignaturesVerified: true, minSdk: 26, targetSdk: 36, versionCode: 1, entryCount: 1, expandedBytes: 1, deviceValidated: false }));
  entries.push(entry);
}
await writeFile(`/work/${name}.json`, JSON.stringify(entries));
await createDownloadBundle(`/work/${name}.json`, dir, `/work/bundle-${name}`);
console.log(JSON.stringify({ bundle: name, releases: entries.length }));
EOF

# The build context: server/ without dependencies, tests or the checkout's bundle.
context() {
  rm -rf "$work/ctx"; mkdir "$work/ctx"
  (cd "$repo/server" && tar -cf - --exclude=./node_modules --exclude=./release-bundle --exclude=./test .) | tar -C "$work/ctx" -xf -
  cp -R "$1" "$work/ctx/release-bundle"
}

if [ -n "${MULTITAP_SMOKE_BUNDLE:-}" ]; then
  cp -R "$MULTITAP_SMOKE_BUNDLE" "$work/bundle-a"; say "image A uses the prepared bundle $MULTITAP_SMOKE_BUNDLE"
else
  node_run /work/make-bundle.mjs a 0.0.1 >/dev/null
fi
node_run /work/make-bundle.mjs b 0.0.2 >/dev/null
# The validator the image runs, here before spending a build.
node_run /app/src/download-bundle.mjs validate /work/bundle-a >/dev/null || fail "bundle A does not validate"

for name in a b; do
  context "$work/bundle-$name"
  say "building $image:$name"
  docker build -q -t "$image:$name" "$work/ctx" >/dev/null
done

# $2 = bootstrap: no address yet, only the server address for the first setup.
# $2 = http: the LAN HTTP address alone, as install.sh starts a server.
# Otherwise an HTTPS address (no proxy here, so only its refusal over plain
# HTTP is observable) with the LAN HTTP address beside it.
write_env() {
  if [ "${2:-}" = bootstrap ]; then addresses="MULTITAP_ALLOW_INSECURE_HTTP=0"
  elif [ "${2:-}" = http ]; then addresses="MULTITAP_HTTP_ORIGIN=$origin
MULTITAP_ALLOW_INSECURE_HTTP=1"
  else addresses="MULTITAP_PUBLIC_ORIGIN=https://smoke.invalid
MULTITAP_HTTP_ORIGIN=$origin
MULTITAP_ALLOW_INSECURE_HTTP=1"; fi
  cat >"$env_file" <<EOF
MULTITAP_IMAGE=$image
MULTITAP_VERSION=$1
MULTITAP_SERVER_ADDRESS=$address
$addresses
MULTITAP_PUBLISH_PORT=0.0.0.0:$port
MULTITAP_UDP_PUBLISH_PORT=$udp_port
MULTITAP_DATA_VOLUME=$volume
MULTITAP_DOCKER_UPDATE_FEED=off
EOF
}

wait_healthy() {
  i=0
  while :; do
    cid=$(compose ps -q broker)
    state=$( [ -n "$cid" ] && docker inspect -f '{{.State.Health.Status}}' "$cid" 2>/dev/null || echo starting)
    [ "$state" = healthy ] && break
    [ "$state" = unhealthy ] && fail "container health check failed"
    i=$((i + 1)); [ "$i" -gt 120 ] && fail "container did not become healthy"
    sleep 2
  done
  curl -fsS "$origin/healthz" | grep -q '"service":"multitap-broker"' || fail "/healthz did not answer"
}

# JSON checks run in the broker container's Node (stdin only, no network).
check_json() { compose exec -T broker node --input-type=module -e "$1"; }
catalog() { curl -fsS "$origin/api/v1/downloads"; }

status_of() { curl -sS -o /dev/null -w '%{http_code}' "$@"; }

write_env a bootstrap
say "starting image A without an address (first setup by IP)"
compose up -d >/dev/null
wait_healthy
compose logs --no-color broker | grep -q '"event":"initial_setup_code"' || fail "no one-time setup code in the log"
compose logs --no-color broker | grep '"event":"initial_setup_url"' | grep -q "처음 설정: $origin 를 브라우저로 여세요" || fail "the log does not name the setup address"
[ "$(curl -sS -o /dev/null -w '%{http_code} %{redirect_url}' "$origin/")" = "302 $origin/admin/" ] || fail "the root address does not open the first setup"
[ "$(status_of -H 'Accept: text/html' "$origin/admin/")" = 200 ] || fail "the setup page does not load"
for path in /api/v1/downloads /download /get/client; do [ "$(status_of "$origin$path")" = 403 ] || fail "$path is open before setup"; done
[ "$(status_of -H "Host: 127.0.0.2:$port" "$origin/admin/")" = 403 ] || fail "another address reaches the setup"
curl -fsS "$origin/healthz" | grep -q '"bootstrapOnly":true' || fail "the setup address is not bootstrap-only"
say "first setup opens at $origin; addressing HTTPS with HTTP beside it"

write_env a
compose up -d >/dev/null
wait_healthy
# What the Windows client's connection test reads for an http:// address.
curl -fsS "$origin/healthz" | grep -q "\"transport\":{\"protocol\":\"broker-transport-v1\",\"publicOrigin\":\"$origin\",\"scheme\":\"http\",\"allowInsecureHttp\":true,\"directTls\":false}" || fail "the HTTP health profile is wrong"
[ "$(status_of -H 'Host: smoke.invalid' "$origin/api/v1/downloads")" = 403 ] || fail "the HTTPS address answers plain HTTP"
compose logs --no-color broker | grep -q '"code":"cleartext_http_enabled"' || fail "no cleartext HTTP warning in the log"
compose logs --no-color broker | grep '"event":"initial_setup_url"' | tail -n 1 | grep -q "https://smoke.invalid 또는 $origin" || fail "the setup line does not name both addresses"
[ "$(curl -sS -o /dev/null -w '%{http_code} %{redirect_url}' -H 'Accept: text/html' "$origin/")" = "302 $origin/admin/" ] || fail "the root does not open a pending setup"

# UDP media relay: started on 18481, bound in the container, published by Docker.
# (It answers only a BIND that carries a session ticket, so without a real Host
# and client nothing can be asked to reply; arrival is not checked here.)
compose logs --no-color broker | grep -q '"event":"udp_relay_started","port":18481' || fail "UDP relay did not start on 18481"
compose exec -T broker cat /proc/net/udp </dev/null | grep -q ' 00000000:4831 00000000:0000 07 ' || fail "nothing bound to UDP 18481 in the container"
case "$(compose port --protocol udp broker 18481)" in *":$udp_port") ;; *) fail "UDP 18481 is not published on $udp_port";; esac

catalog >"$work/catalog-a.json"
check_json '
let t="";for await(const c of process.stdin)t+=c;const r=JSON.parse(t).releases;
for(const p of ["client","host","android-client"])if(!r.some(e=>e.product===p))throw Error("missing "+p);
const abis=new Set(r.filter(e=>e.product==="android-client").map(e=>e.abi));
if(abis.size!==3)throw Error("Android ABIs missing");' <"$work/catalog-a.json" || fail "/api/v1/downloads lacks client, Host or Android"

for key in client host android; do
  location=$(curl -sS -o /dev/null -w '%{redirect_url}' "$origin/get/$key")
  case "$location" in "$origin/downloads/"*) ;; *) fail "/get/$key does not lead to a download ($location)";; esac
done
first=$(curl -sS -o /dev/null -w '%{redirect_url}' "$origin/get/android")
curl -fsS "$first" >"$work/first.bin"
actual=$(sha256sum "$work/first.bin" | cut -d' ' -f1)
grep -q "\"filename\":\"${first##*/}\",\"size\":[0-9]*,\"sha256\":\"$actual\"" "$work/catalog-a.json" || fail "downloaded file does not match its catalog SHA-256"

vault_hash() { compose exec -T broker node -e 'process.stdout.write(require("crypto").createHash("sha256").update(require("fs").readFileSync("/var/lib/multitap/vault.key")).digest("hex"))' </dev/null; }
vault_a=$(vault_hash)
say "image A serves client, Host and Android; switching the volume to image B"

write_env b
compose up -d >/dev/null
wait_healthy
compose logs --no-color broker | grep -q 'Existing users preserved.' || fail "accounts were not preserved"
[ "$(vault_hash)" = "$vault_a" ] || fail "vault key changed"
catalog >"$work/catalog-b.json"
# Every A release is still listed with the same hash and downloads byte-identical.
for row in $(grep -o '"filename":"[^"]*","size":[0-9]*,"sha256":"[a-f0-9]*"' "$work/catalog-a.json" | sed 's/"filename":"\([^"]*\)","size":[0-9]*,"sha256":"\([a-f0-9]*\)"/\1=\2/'); do
  name=${row%%=*}; sum=${row#*=}
  grep -q "\"filename\":\"$name\"" "$work/catalog-b.json" || fail "$name disappeared from the catalog"
  [ "$(curl -fsS "$origin/downloads/$name" | sha256sum | cut -d' ' -f1)" = "$sum" ] || fail "$name changed"
done
grep -q '"version":"0.0.2"' "$work/catalog-b.json" || fail "image B releases were not added"
say "image B kept every earlier file and added its own"

compose restart broker >/dev/null
wait_healthy
catalog >"$work/catalog-c.json"
cmp -s "$work/catalog-b.json" "$work/catalog-c.json" || fail "a plain restart changed the catalog"

# ---- server access: a change saved the way the console saves it ---------------
# The server starts from the LAN HTTP address alone, as install.sh leaves it.
write_env b http
compose up -d >/dev/null
wait_healthy
say "server access: first setup, then a save through the admin API"

jar="$work/cookies"
csrf=''
password="Smoke-$id-pass"
credentials="{\"username\":\"smoke-admin\",\"password\":\"$password\"}"
# One value of the JSON on stdin by its dotted path (a list joined with commas),
# read by the Node in the server's container.
json() {
  compose exec -T broker node -e 'let t="";process.stdin.on("data",d=>t+=d).on("end",()=>{let v=JSON.parse(t);for(const k of process.argv[1].split("."))v=v?.[k];process.stdout.write(Array.isArray(v)?v.join(","):v===null||v===undefined?"":typeof v==="object"?JSON.stringify(v):String(v))})' "$1"
}
api() { # api <method> <path> [json body]: the answer's body
  if [ -n "${3:-}" ]; then
    curl -sS -b "$jar" -c "$jar" -X "$1" -H "Origin: $origin" -H "X-CSRF-Token: $csrf" -H 'Content-Type: application/json' --data "$3" "$origin/api/v1/admin$2"
  else
    curl -sS -b "$jar" -c "$jar" -X "$1" -H "Origin: $origin" -H "X-CSRF-Token: $csrf" "$origin/api/v1/admin$2"
  fi
}
sign_in() { # sign_in <json credentials>
  csrf=''
  csrf=$(api POST /login "$1" | json csrfToken)
  [ -n "$csrf" ] || fail "sign-in failed"
}
access() { api GET /server-access; }
# Until the console reports this state. The server restarts itself in between
# and a saved policy gives each address its own sign-in, so it signs in again
# whenever the answer is not a status.
wait_access() { # wait_access <state>
  i=0
  state=''
  while :; do
    state=$(access 2>/dev/null | json state 2>/dev/null || true)
    [ "$state" = "$1" ] && return 0
    if [ -z "$state" ]; then csrf=$(api POST /login "$credentials" 2>/dev/null | json csrfToken 2>/dev/null || true); fi
    i=$((i + 1))
    [ "$i" -gt 60 ] && fail "server access did not reach \"$1\" (last: ${state:-no answer})"
    sleep 2
  done
}
# {config, baseRevision} for a test: the applied settings on stdin with one change (JavaScript on `c`).
changed_config() {
  compose exec -T broker node -e 'let t="";process.stdin.on("data",d=>t+=d).on("end",()=>{const s=JSON.parse(t),c=s.applied;eval(process.argv[1]);process.stdout.write(JSON.stringify({config:c,baseRevision:s.revision}))})' "$1"
}
save_access() { # save_access <change>: test, then save, as the page does
  body=$(access | changed_config "$1")
  test_id=$(api POST /server-access/test "$body" | json testId)
  [ -n "$test_id" ] || fail "the server access test was refused"
  body=$(printf '%s' "$body" | compose exec -T broker node -e 'let t="";process.stdin.on("data",d=>t+=d).on("end",()=>process.stdout.write(JSON.stringify({...JSON.parse(t),testId:process.argv[1],confirm:true})))' "$test_id")
  api POST /server-access/save "$body" | grep -q '"accepted":true' || fail "the server access save was refused"
}
# The server's own command for the same settings (what `multitap address` runs).
cli() { compose run --rm --no-deps -T --entrypoint node broker /app/src/access-cli.mjs "$@" 2>/dev/null </dev/null; }
starts() { compose logs --no-color broker | grep -c '"service":"multitap-broker","version"' || true; }

code=$(compose logs --no-color broker | grep -o '"setupCode":"[^"]*"' | tail -n 1 | cut -d'"' -f4)
[ -n "$code" ] || fail "no setup code in the log"
sign_in '{"username":"admin","password":"admin"}'
api POST /setup "{\"username\":\"smoke-admin\",\"password\":\"$password\",\"passwordConfirmation\":\"$password\",\"setupCode\":\"$code\"}" | grep -q '"requiresLogin":true' || fail "the first setup did not complete"
sign_in "$credentials"

access >"$work/access-0.json"
[ "$(json selfManaged <"$work/access-0.json")" = true ] || fail "this install does not manage its own access settings"
[ "$(json applied.primaryOrigin <"$work/access-0.json")" = "$origin" ] || fail "the address from .env is not the applied one"
# UDP: the server is told the published port and gives each side its own address at that port.
[ "$(compose exec -T broker printenv MULTITAP_UDP_PUBLISHED_PORT </dev/null)" = "$udp_port" ] || fail "the container is not told the published UDP port"
[ "$(json udpMedia.derivedPort <"$work/access-0.json")" = "$udp_port" ] || fail "no UDP address is derived from the published port"
[ "$(status_of "$loopback/api/v1/downloads")" = 403 ] || fail "an address that is not listed answers"

cid=$(compose ps -q broker)
started=$(docker inspect -f '{{.State.StartedAt}}' "$cid")
before=$(starts)
# 1. One more HTTP address. The server writes the policy, starts again in the
#    same container, reaches its main address and keeps the change.
save_access "c.http={...c.http,extraOrigins:[...(c.http.extraOrigins??[]),'$loopback']}"
wait_access applied
[ "$(compose ps -q broker)" = "$cid" ] || fail "the container was replaced by a save"
[ "$(docker inspect -f '{{.State.StartedAt}} {{.RestartCount}}' "$cid")" = "$started 0" ] || fail "the container itself was restarted by a save"
[ "$(starts)" -eq $((before + 1)) ] || fail "the server process was not started again exactly once"
[ "$(status_of "$loopback/api/v1/downloads")" = 200 ] || fail "the saved address does not answer"
[ "$(status_of "$origin/api/v1/downloads")" = 200 ] || fail "the main address stopped answering"
compose exec -T broker sh -c 'test "$(stat -c %u:%a /var/lib/multitap/access/policy.json)" = 1000:600' </dev/null || fail "the saved policy is not the server account's own file"
cli show >"$work/show-1.txt" || fail "access-cli show failed"
grep -qx 'source=saved' "$work/show-1.txt" || fail "access-cli does not see the saved policy"
grep -qx "http=$origin,$loopback" "$work/show-1.txt" || fail "access-cli shows other addresses than the console saved"
say "a save was applied by the server itself, in the same container"

# 2. A new main address that cannot answer (no such name): the server starts
#    with it, cannot reach it, puts the previous policy back and starts again.
before=$(starts)
save_access "c.primaryOrigin='https://smoke.invalid';c.https={...c.https,enabled:true,origin:'https://smoke.invalid'}"
wait_access failed_rolled_back
access >"$work/access-2.json"
[ "$(json applied.primaryOrigin <"$work/access-2.json")" = "$origin" ] || fail "the previous primary address is not back"
[ "$(json applied.https.enabled <"$work/access-2.json")" = false ] || fail "the refused HTTPS address stayed"
[ "$(starts)" -eq $((before + 2)) ] || fail "going back did not take exactly two starts"
[ "$(compose ps -q broker)" = "$cid" ] || fail "the container was replaced while going back"
[ "$(status_of "$loopback/api/v1/downloads")" = 200 ] || fail "the previous addresses do not answer after going back"
cli show >"$work/show-2.txt" || fail "access-cli show failed"
grep -qx "http=$origin,$loopback" "$work/show-2.txt" || fail "access-cli shows other addresses after going back"
grep -qx 'pending=' "$work/show-2.txt" || fail "a change is still pending after going back"
say "a save whose address cannot answer was put back by the server"

# 3. The same policy from the command line: set replaces it (read at the next
#    start), revert puts back what set replaced.
cli set "$origin" | grep -qx 'changed=1' || fail "access-cli set did not change the policy"
compose restart broker >/dev/null
wait_healthy
[ "$(status_of "$loopback/api/v1/downloads")" = 403 ] || fail "an address removed by access-cli still answers"
cli revert | grep -qx 'ok=1' || fail "access-cli revert failed"
compose restart broker >/dev/null
wait_healthy
[ "$(status_of "$loopback/api/v1/downloads")" = 200 ] || fail "access-cli revert did not bring the address back"
say "access-cli reads and writes the policy the console saved"

# ---- the multitap command itself (root only) -----------------------------------
# It manages the install in MULTITAP_DIR: a folder with this compose.yaml and
# the same settings, tied to this project by name. Checked first: without that
# name it would act on a project called "multitap".
install_dir="$work/install"
mkdir -p "$install_dir/backups"
cp "$repo/compose.yaml" "$install_dir/compose.yaml"
# The command confirms a release by the image's own version label, so the
# install names image B by its real version, as a published image is named.
docker tag "$image:b" "$image:$version"
{ grep -v '^MULTITAP_VERSION=' "$env_file"; printf 'MULTITAP_VERSION=%s\nCOMPOSE_PROJECT_NAME=%s\nMULTITAP_LANG=en\n' "$version" "$project"; } >"$install_dir/.env"
multitap() { MULTITAP_DIR="$install_dir" MULTITAP_UNIT_DIR="$work/units" MULTITAP_LOCAL=1 NO_COLOR=1 bash "$repo/deploy/docker/multitap" "$@"; }
if [ "$(id -u)" != 0 ] || ! command -v bash >/dev/null; then
  say "multitap command: skipped (needs root and bash)"
elif [ "$(cd "$install_dir" && docker compose config 2>/dev/null | sed -n 's/^name: //p' | head -n 1)" != "$project" ]; then
  say "multitap command: skipped (the folder is not tied to project $project)"
else
  mkdir -p "$work/units"
  multitap status >"$work/multitap-status.txt" || fail "multitap status failed"
  grep -q 'is running and answering' "$work/multitap-status.txt" || fail "multitap status does not see the server answering"
  grep -q "$origin (primary)" "$work/multitap-status.txt" || fail "multitap status does not show the saved primary address"
  multitap address | grep -q "$loopback" || fail "multitap address does not list the saved addresses"
  # HTTPS only: no HTTP address answers any more, and the command's wait still ends.
  multitap address https://smoke.invalid >"$work/multitap-address.txt" || { cat "$work/multitap-address.txt" >&2; fail "multitap address (HTTPS only) failed"; }
  [ "$(status_of "$origin/api/v1/downloads")" = 403 ] || fail "HTTP still answers under an HTTPS-only policy"
  multitap restart >/dev/null || fail "multitap restart did not see the HTTPS-only server answering"
  multitap status | grep -q 'https://smoke.invalid (primary)' || fail "multitap status does not show the HTTPS-only policy"
  grep -q '^MULTITAP_HTTP_ORIGIN=$' "$install_dir/.env" || fail "multitap address left the address lines in .env"
  multitap address "$origin" >/dev/null || fail "multitap address could not bring the HTTP address back"
  [ "$(status_of "$origin/api/v1/downloads")" = 200 ] || fail "the HTTP address does not answer after multitap address"
  multitap backup >/dev/null || fail "multitap backup failed"
  ls "$install_dir"/backups/multitap-data-*.tar.gz >/dev/null 2>&1 || fail "multitap backup left no copy"
  say "multitap status, address, restart and backup work against this server"
fi
say "PASS"
