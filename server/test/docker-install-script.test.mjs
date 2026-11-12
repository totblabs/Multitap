// deploy/docker/install.sh, run from start to finish on the machine of
// docker-command-fixture.mjs. The script runs as a copy whose fixed system
// paths (/etc/os-release, /var/log, /usr/local/bin, /etc/apt, /dev/tty, the
// two /proc and /dev files it reads in a CT) point into the test folder;
// everything else is the script as it is. apt, docker, curl and systemctl are
// fakes, so this shows the script's own flow: what it asks, writes, records
// and says. What it cannot show is a real Debian, apt, Docker or a Proxmox CT.
// Needs bash (Git Bash on Windows); skipped without it.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { writeFileSync, readFileSync, existsSync, mkdirSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { VERSION } from '../src/restore-cli.mjs';
import { bash, posix, commandMachine } from './docker-command-fixture.mjs';

const skip = !bash && 'no bash';
const repository = name => fileURLToPath(new URL('../../' + name, import.meta.url));
const setupCode = 'ABCD-EFGH-JKLM';
const extraFakes = {
  'apt-get': `#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$FAKE_ROOT/apt-calls"
case " $* " in
  *" update "*) if [ "\${FAKE_APT_FAIL:-}" = update ]; then echo "E: Failed to fetch http://deb.debian.org/debian/dists/bookworm/InRelease  Temporary failure resolving 'deb.debian.org'" >&2; exit 100; fi ;;
  *" docker-ce "*) [ -z "\${FAKE_NO_DOCKER_REPOSITORY:-}" ] || { echo "E: Unable to locate package docker-ce" >&2; exit 100; }; touch "$FAKE_ROOT/compose-plugin" ;;
  *" docker-compose-v2 "*) [ -n "\${FAKE_HAS_COMPOSE_V2:-}" ] || { echo "E: Unable to locate package docker-compose-v2" >&2; exit 100; }; touch "$FAKE_ROOT/compose-plugin" ;;
  # Only a docker-compose that is Compose v2 brings the plugin.
  *" docker-compose "*) case "\${FAKE_COMPOSE_CANDIDATE:-}" in 2.*) touch "$FAKE_ROOT/compose-plugin" ;; esac ;;
esac
`,
  'apt-cache': '#!/usr/bin/env bash\nprintf \'docker-compose:\\n  Installed: (none)\\n  Candidate: %s\\n\' "${FAKE_COMPOSE_CANDIDATE:-(none)}"\n',
  'systemd-detect-virt': '#!/usr/bin/env bash\nif [ -n "${FAKE_CT:-}" ]; then echo lxc; else echo none; fi\n',
  ip: '#!/usr/bin/env bash\necho "1.1.1.1 via 192.0.2.1 dev eth0 src 192.0.2.20 uid 0"\n',
  mount: '#!/usr/bin/env bash\n[ -n "${FAKE_MOUNT_OK:-}" ]\n',
  umount: '#!/usr/bin/env bash\nexit 0\n',
  timedatectl: '#!/usr/bin/env bash\necho Europe/Berlin\n',
};

function installer(t) {
  const m = commandMachine(t, { version: VERSION, prefix: 'multitap-docker-install-' });
  const bin = join(m.root, 'bin'), system = join(m.root, 'system'), local = join(m.root, 'local'), dir = join(m.root, 'installed');
  for (const path of [system, local, join(system, 'bin'), join(system, 'apt')]) mkdirSync(path, { recursive: true });
  for (const [name, text] of Object.entries(extraFakes)) writeFileSync(join(bin, name), text, { mode: 0o755 });
  writeFileSync(join(system, 'os-release'), 'ID=debian\nVERSION_CODENAME=bookworm\nPRETTY_NAME="Debian GNU/Linux 12 (bookworm)"\n');
  // An answer file stands for the terminal: writing a question empties it, so every answer read is Enter.
  writeFileSync(join(system, 'tty'), '');
  // What a release publishes, as MULTITAP_LOCAL takes it.
  for (const [from, name] of [['compose.yaml', 'compose.yaml'], ['.env.example', '.env.example'], ['deploy/docker/multitap', 'multitap']]) writeFileSync(join(local, name), readFileSync(repository(from), 'utf8').replaceAll('\r\n', '\n'));
  writeFileSync(join(m.fake, 'logs'), `{"event":"initial_setup_code","setupCode":"${setupCode}"}\n{"service":"multitap-broker","listeners":[]}\n`);
  const paths = { '/etc/os-release': join(system, 'os-release'), '/var/log/multitap-install.log': join(system, 'install.log'), '/usr/local/bin/multitap': join(system, 'bin', 'multitap'), '/etc/apt/keyrings': join(system, 'apt', 'keyrings'),
    '/etc/apt/sources.list.d': join(system, 'apt'), '/dev/tty': join(system, 'tty'), '/proc/self/uid_map': join(system, 'uid_map'), '/dev/.lxc/proc': join(system, 'lxc-proc') };
  let text = readFileSync(repository('deploy/docker/install.sh'), 'utf8').replaceAll('\r\n', '\n');
  for (const [from, to] of Object.entries(paths)) { assert.ok(text.includes(from), from); text = text.replaceAll(from, posix(to)); }
  const copy = join(m.root, 'install.sh');
  writeFileSync(copy, text);
  const run = (extra = {}) => {
    const result = spawnSync(bash, ['-c', 'PATH="$FAKE_BIN:$PATH"; exec bash "$SCRIPT"', 'install'], { encoding: 'utf8', input: '', timeout: 120000,
      env: { ...process.env, LANG: 'C.UTF-8', NO_COLOR: '1', MULTITAP_LANG: 'en', MULTITAP_LOCAL: posix(local), MULTITAP_DIR: posix(dir), MULTITAP_UNIT_DIR: posix(m.units), FAKE_ROOT: posix(m.fake), FAKE_APP: posix(fileURLToPath(new URL('..', import.meta.url))),
        FAKE_BIN: posix(bin), SCRIPT: posix(copy), NODE: posix(process.execPath), FAKE_VERSION: VERSION, ...extra } });
    const log = existsSync(join(system, 'install.log')) ? readFileSync(join(system, 'install.log'), 'utf8') : '';
    return { status: result.status, out: result.stdout + result.stderr, log, calls: m.read('calls'), apt: m.read('apt-calls'), curl: m.read('curl-calls') };
  };
  const dotenv = () => Object.fromEntries(readFileSync(join(dir, '.env'), 'utf8').split('\n').filter(line => /^[A-Z_]+=/.test(line)).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
  return { ...m, dir, system, run, dotenv };
}

test('a new server: Enter opens the LAN address and leaves plain HTTP from outside closed; the record holds the run without the setup code', { skip }, t => {
  const m = installer(t);
  const result = m.run({ FAKE_PUBLIC_IP: '203.0.113.7' });
  assert.equal(result.status, 0, result.out);
  const env = m.dotenv();
  assert.equal(env.MULTITAP_VERSION, VERSION); assert.equal(env.MULTITAP_SERVER_ADDRESS, '192.0.2.20'); assert.equal(env.TZ, 'Europe/Berlin'); assert.equal(env.MULTITAP_LANG, 'en');
  assert.equal(env.MULTITAP_HTTP_ORIGIN, 'http://192.0.2.20:18480', 'the public address is not listed'); assert.equal(env.MULTITAP_ALLOW_INSECURE_HTTP, '1');
  assert.match(result.out, /HTTP from outside \(203\.0\.113\.7\) is not opened\. To open it: admin console > Server access/);
  // The closing lines: the address the server itself has, the code, and both ways in from outside.
  assert.match(result.out, /Open in a browser\s+http:\/\/192\.0\.2\.20:18480/); assert.match(result.out, new RegExp('Setup code\\s+' + setupCode)); assert.match(result.out, /In apps and Hosts, enter http:\/\/192\.0\.2\.20:18480/);
  assert.match(result.out, /Recommended: a domain with HTTPS\./); assert.match(result.out, /Turn on “Follow the public IP” on the same page\./);
  assert.doesNotMatch(result.out, /From outside: http/);
  assert.equal(existsSync(join(m.dir, '.installed')), true); assert.equal(existsSync(join(m.dir, 'public-ip')), false); assert.equal(m.policy(), null);
  // The record: every step, no colour codes, and nowhere the setup code.
  for (const said of ['[1/4] Checking the system', '[2/4] Docker', 'Docker is already installed', '[3/4] Server files and image', '[4/4] Starting the server', 'Server ' + VERSION + ' is answering', 'Installed: Multitap server ' + VERSION]) assert.ok(result.log.includes(said), said + '\n' + result.log);
  assert.equal(result.log.includes(setupCode), false); assert.doesNotMatch(result.log, /\x1b\[/); assert.doesNotMatch(result.log, /Setup code/);
  assert.doesNotMatch(result.curl, /healthz/, 'the wait reads the container, never an address');
  // Run again: nothing is changed.
  const again = m.run();
  assert.equal(again.status, 0); assert.match(again.out, /Already installed/);
});

test('outside access is opened only when asked for: the address is listed for the first start and the server follows it, with no timer and no restart', { skip }, t => {
  const m = installer(t);
  const result = m.run({ FAKE_PUBLIC_IP: '203.0.113.7', MULTITAP_LAN_HTTP: '1', MULTITAP_PUBLIC_HTTP: '1' });
  assert.equal(result.status, 0, result.out);
  assert.equal(m.dotenv().MULTITAP_HTTP_ORIGIN, 'http://192.0.2.20:18480,http://203.0.113.7:18480');
  assert.deepEqual(m.policy().publicIp, { follow: true, origin: 'http://203.0.113.7:18480' });
  assert.match(result.out, /In apps and Hosts, enter http:\/\/192\.0\.2\.20:18480\n\s+→ From outside: http:\/\/203\.0\.113\.7:18480/); assert.match(result.out, /http:\/\/203\.0\.113\.7:18480 is registered, and the server follows the public IP when it changes\./);
  assert.equal(m.read('systemctl-calls'), ''); assert.equal(existsSync(join(m.dir, 'public-ip')), false);
  assert.equal((result.calls.match(/^compose (up|restart|start)/gm) ?? []).length, 1, 'one start: the setup code that was printed stays valid');
  // Without a terminal nothing unencrypted is opened to the internet unless it was asked for.
  const quiet = installer(t);
  writeFileSync(join(quiet.system, 'tty'), ''); spawnSync(bash, ['-c', 'rm "$1"', 'rm', posix(join(quiet.system, 'tty'))]);
  const unattended = quiet.run({ FAKE_PUBLIC_IP: '203.0.113.7' });
  assert.equal(unattended.status, 0, unattended.out); assert.equal(quiet.dotenv().MULTITAP_HTTP_ORIGIN, 'http://192.0.2.20:18480');
  // What "without a terminal" is on a real machine (ssh without -t, cloud-init): /dev/tty is there, passes the
  // permission tests and cannot be opened. Seen on a real CT, where the first question ended the install. A folder stands for it here.
  const closed = installer(t);
  spawnSync(bash, ['-c', 'rm "$1" && mkdir "$1"', 'tty', posix(join(closed.system, 'tty'))]);
  const piped = closed.run({ FAKE_PUBLIC_IP: '203.0.113.7' });
  assert.equal(piped.status, 0, piped.out); assert.equal(closed.dotenv().MULTITAP_HTTP_ORIGIN, 'http://192.0.2.20:18480');
  assert.match(piped.out, /Address for apps and Hosts: http:\/\/192\.0\.2\.20:18480/); assert.match(piped.out, /HTTP from outside \(203\.0\.113\.7\) is not opened/);
});

test('a step that fails says where, shows the hidden output and names the record', { skip }, t => {
  const m = installer(t);
  const failed = m.run({ FAKE_NO_COMPOSE: '1', FAKE_APT_FAIL: 'update' });
  assert.equal(failed.status, 100, failed.out);
  assert.match(failed.out, /Stopped\n\s+A command failed in step \[Docker\] \(line \d+, exit code 100\): apt-get update -qq\n/); assert.match(failed.out, /The last lines of the log:[\s\S]*Temporary failure resolving 'deb\.debian\.org'/); assert.match(failed.out, /Install log: .*install\.log/);
  assert.match(failed.log, /FAILED in \[Docker\]: line \d+, exit code 100/);
  assert.equal(existsSync(join(m.dir, '.env')), false, 'nothing of the server was written yet');

  // The image cannot be downloaded: Docker's own words, and what kind of failure they name.
  for (const [error, said] of [['Error response from daemon: Head "https://ghcr.io/v2/totblabs/multitap-broker/manifests/' + VERSION + '": denied', /The registry refused \(access denied\)\./],
    ['Error response from daemon: Get "https://ghcr.io/v2/": dial tcp: lookup ghcr.io on 192.0.2.1:53: no such host', /The registry's name could not be resolved \(DNS\)\./],
    ['Error response from daemon: Get "https://ghcr.io/v2/": net/http: request canceled while waiting for connection (Client.Timeout exceeded while awaiting headers)', /The reason is in Docker's message above\./],
    ['Error response from daemon: Get "https://ghcr.io/v2/": dial tcp 192.0.2.9:443: i/o timeout', /The registry did not answer in time\./]]) {
    const n = installer(t);
    for (const name of ['compose.yaml', '.env.example', 'multitap']) copyFileSync(join(n.root, 'local', name), join(n.fake, name));
    const pull = n.run({ MULTITAP_LOCAL: '', MULTITAP_VERSION: VERSION, MULTITAP_LAN_HTTP: '0', FAKE_NO_IMAGE: '1', FAKE_PULL_ERROR: error, FAKE_RELEASE_FILES: '1' });
    assert.equal(pull.status, 1, pull.out);
    assert.ok(pull.out.includes(error), pull.out); assert.match(pull.out, said); assert.match(pull.out, /Install log: /);
    assert.doesNotMatch(pull.out, /Check the internet connection/);
  }
});

test('Docker from the distribution: Compose v1 is not installed and the stop says what is missing; a v2 package is used', { skip }, t => {
  // Docker's own repository cannot be reached (the fake curl refuses download.docker.com).
  const v1 = installer(t);
  const stopped = v1.run({ FAKE_NO_COMPOSE: '1', FAKE_COMPOSE_CANDIDATE: '1.29.2-3' });
  assert.equal(stopped.status, 1, stopped.out);
  assert.match(stopped.out, /this distribution's packages have no 'docker compose' \(Compose v2\)\. Check that download\.docker\.com can be reached/);
  assert.match(stopped.apt, /install -y -qq docker\.io\n/); assert.match(stopped.apt, /install -y -qq docker-compose-v2\n/); assert.doesNotMatch(stopped.apt, /install -y -qq docker-compose\n/, 'the v1 command is not installed for nothing');
  for (const extra of [{ FAKE_HAS_COMPOSE_V2: '1' }, { FAKE_COMPOSE_CANDIDATE: '2.26.1-1' }]) {
    const m = installer(t);
    const result = m.run({ FAKE_NO_COMPOSE: '1', MULTITAP_LAN_HTTP: '1', ...extra });
    assert.equal(result.status, 0, JSON.stringify(extra) + '\n' + result.out); assert.match(result.out, /Docker installed \(27\.0\.0\)/);
  }
});

test('a Proxmox CT without nesting is stopped before anything is installed, with the setting to change', { skip }, t => {
  const m = installer(t);
  const stopped = m.run({ FAKE_CT: '1', FAKE_NO_COMPOSE: '1' });
  assert.equal(stopped.status, 1, stopped.out);
  assert.match(stopped.out, /Nesting is off in this CT, so Docker cannot run containers here\. Nothing was installed yet\./); assert.match(stopped.out, /turn on nesting, then restart the CT\./); assert.match(stopped.out, /pct set <CT number> --features nesting=1 && pct reboot <CT number>/);
  assert.equal(stopped.apt, '', 'apt was not touched'); assert.equal(existsSync(m.dir), false);
  // An unprivileged CT (root mapped to another user on the node) needs keyctl too.
  writeFileSync(join(m.system, 'uid_map'), '         0     100000      65536\n');
  assert.match(m.run({ FAKE_CT: '1' }).out, /turn on nesting and keyctl, then restart the CT\.[\s\S]*--features nesting=1,keyctl=1/);
  // Either sign of nesting lets the install go on; the check can be skipped.
  for (const extra of [{ FAKE_MOUNT_OK: '1' }, { MULTITAP_CT_CHECK: '0' }]) {
    const n = installer(t);
    const result = n.run({ FAKE_CT: '1', MULTITAP_LAN_HTTP: '1', ...extra });
    assert.equal(result.status, 0, JSON.stringify(extra) + '\n' + result.out); assert.match(result.out, /Running in a Proxmox CT/);
  }
  const nested = installer(t);
  mkdirSync(join(nested.system, 'lxc-proc'));
  assert.equal(nested.run({ FAKE_CT: '1', MULTITAP_LAN_HTTP: '1' }).status, 0);
  // The backstop: the container that will not start, with Docker's own words.
  const late = installer(t);
  const failed = late.run({ FAKE_CT: '1', FAKE_MOUNT_OK: '1', MULTITAP_LAN_HTTP: '1', FAKE_RUN_ERROR: 'docker: Error response from daemon: failed to create task for container: OCI runtime create failed: open sysctl net.ipv4.ip_unprivileged_port_start file: permission denied' });
  assert.equal(failed.status, 1); assert.match(failed.out, /OCI runtime create failed/); assert.match(failed.out, /turn on nesting under CT > Options > Features \(and keyctl for an unprivileged CT\)/);
});
