// A machine for the tests of the `multitap` command (deploy/docker/multitap):
// an install folder, a folder that stands for the data volume, and small fakes
// in front of the real programs. `docker` is fake-docker.sh: `docker run` of
// the server image runs this checkout's code, tar and find directly on the
// volume folder; `docker compose` records whether the server runs and with
// which release and settings its container was created; `docker inspect`
// answers the state and health line the command waits on. `curl` answers no
// health address at all: the command's wait must never need one.
// What this cannot show is real Docker: the image, its user, mounts, the
// health check itself and the container's own limits.
// Needs bash (Git Bash on Windows); the tests skip without it.
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { temporaryDirectory } from './backup-fixture.mjs';

export const serverDir = fileURLToPath(new URL('..', import.meta.url)), script = fileURLToPath(new URL('../../deploy/docker/multitap', import.meta.url));
function findBash() {
  if (process.env.MULTITAP_TEST_BASH) return process.env.MULTITAP_TEST_BASH;
  const candidates = process.platform === 'win32' ? ['C:\\Program Files\\Git\\bin\\bash.exe', fileURLToPath(new URL('../../.tools/git/bin/bash.exe', import.meta.url))] : ['/bin/bash', '/usr/bin/bash'];
  return candidates.find(path => existsSync(path)) ?? null;
}
export const bash = findBash();
// Git Bash wants /c/... paths (a "C:" would read as a remote tar host).
export const posix = path => process.platform === 'win32' ? '/' + path[0].toLowerCase() + path.slice(2).replaceAll('\\', '/') : path;

const fakes = {
  // Read as text, so a checkout with CRLF line ends still gives bash LF.
  docker: readFileSync(fileURLToPath(new URL('./fake-docker.sh', import.meta.url)), 'utf8').replaceAll('\r\n', '\n').replace(/^/, '#!/usr/bin/env bash\n'),
  // Every request is recorded. The public IP echo answers FAKE_PUBLIC_IP, a
  // release's compose.yaml is the file the test put in place; everything
  // else, every /healthz included, is refused.
  curl: `#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$FAKE_ROOT/curl-calls"
url='' out=''
while [ $# -gt 0 ]; do case "$1" in -o) out=$2; shift 2 ;; -w|-H|--max-time) shift 2 ;; -*) shift ;; *) url=$1; shift ;; esac; done
case "$url" in
  https://1.1.1.1/cdn-cgi/trace) [ -n "\${FAKE_PUBLIC_IP:-}" ] || exit 7; printf 'fl=1\\nip=%s\\nts=1\\n' "$FAKE_PUBLIC_IP" ;;
  # The newest public release, as the redirect of releases/latest names it.
  https://github.com/*/releases/latest) [ -n "\${FAKE_LATEST:-}" ] || exit 7; printf 'https://github.com/totblabs/Multitap/releases/tag/v%s' "$FAKE_LATEST" ;;
  https://raw.githubusercontent.com/*/compose.yaml) if [ -f "$FAKE_ROOT/release-compose.yaml" ]; then cp "$FAKE_ROOT/release-compose.yaml" "$out"; elif [ -n "\${FAKE_RELEASE_FILES:-}" ]; then cp "$FAKE_ROOT/compose.yaml" "$out"; else exit 22; fi ;;
  # The other files of a release only for the install test, which sends them into its own folder.
  https://raw.githubusercontent.com/*) [ -n "\${FAKE_RELEASE_FILES:-}" ] && [ -f "$FAKE_ROOT/\${url##*/}" ] || exit 22; cp "$FAKE_ROOT/\${url##*/}" "$out" ;;
  *) exit 7 ;;
esac
`,
  systemctl: '#!/usr/bin/env bash\nprintf \'%s\\n\' "$*" >> "$FAKE_ROOT/systemctl-calls"\n',
  // Free space as the test says, else the real answer.
  df: '#!/usr/bin/env bash\nif [ -n "${FAKE_DF_FREE_KB:-}" ]; then printf \'Filesystem 1024-blocks Used Available Capacity Mounted on\\n/dev/fake 100000000 1 %s 1%% /\\n\' "$FAKE_DF_FREE_KB"; else command -p df "$@"; fi\n',
  id: '#!/usr/bin/env bash\necho 0\n',
  chown: '#!/usr/bin/env bash\nexit 0\n',
  sleep: '#!/usr/bin/env bash\nexit 0\n',
  // The broker's account does not exist here, and Windows folders take no Unix modes: owner, group
  // and mode are dropped (what they would be is not tested here).
  install: '#!/usr/bin/env bash\ndir=no; paths=(); skip=no\nfor a in "$@"; do if [ $skip = yes ]; then skip=no; continue; fi; case "$a" in -o|-g|-m) skip=yes ;; -d) dir=yes ;; *) paths+=("$a") ;; esac; done\nif [ $dir = yes ]; then mkdir -p "${paths[@]}"; else cp "${paths[0]}" "${paths[1]}"; fi\n',
};

// `env` is the install's .env; `version` the release it runs.
export function commandMachine(t, { version, env = [], prefix = 'multitap-docker-command-' }) {
  const root = temporaryDirectory(t, prefix), opt = join(root, 'opt'), fake = join(root, 'fake'), bin = join(root, 'bin'), volume = join(fake, 'volume'), units = join(root, 'units');
  for (const path of [opt, join(opt, 'backups'), fake, volume, bin, units]) mkdirSync(path, { recursive: true });
  for (const [name, text] of Object.entries(fakes)) writeFileSync(join(bin, name), text, { mode: 0o755 });
  writeFileSync(join(opt, '.env'), [`MULTITAP_VERSION=${version}`, 'MULTITAP_LANG=en', ...env, ''].join('\n'));
  writeFileSync(join(opt, 'compose.yaml'), 'name: multitap\n');
  writeFileSync(join(fake, 'state'), 'running\n'); writeFileSync(join(fake, 'container-version'), version + '\n');
  for (const name of ['calls', 'curl-calls', 'systemctl-calls']) writeFileSync(join(fake, name), '');
  const read = name => { try { return readFileSync(join(fake, name), 'utf8'); } catch (error) { if (error.code === 'ENOENT') return ''; throw error; } };
  const run = (args, extra = {}) => {
    const result = spawnSync(bash, ['-c', 'PATH="$FAKE_BIN:$PATH"; exec bash "$SCRIPT" "$@"', 'multitap', ...args], { encoding: 'utf8', input: '', timeout: 120000,
      env: { ...process.env, LANG: 'C.UTF-8', NO_COLOR: '1', MULTITAP_LOCAL: '1', MULTITAP_DIR: posix(opt), MULTITAP_UNIT_DIR: posix(units), FAKE_ROOT: posix(fake), FAKE_APP: posix(serverDir), FAKE_BIN: posix(bin), SCRIPT: posix(script), NODE: posix(process.execPath), FAKE_VERSION: version, ...extra } });
    return { status: result.status, out: result.stdout + result.stderr, calls: read('calls'), curl: read('curl-calls'), systemctl: read('systemctl-calls'), state: read('state').trim() };
  };
  const dotenv = () => Object.fromEntries(readFileSync(join(opt, '.env'), 'utf8').split('\n').filter(line => line.includes('=')).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
  // The saved access policy of the volume, null while there is none.
  const policy = () => { try { return JSON.parse(readFileSync(join(volume, 'access', 'policy.json'), 'utf8')); } catch (error) { if (error.code === 'ENOENT') return null; throw error; } };
  const backups = () => readdirSync(join(opt, 'backups')).sort();
  return { root, opt, fake, volume, units, run, read, dotenv, policy, backups };
}
