// The `multitap` command (deploy/docker/multitap) on the machine of
// docker-command-fixture.mjs: status, address, public-ip, update, backup and
// the waits between them. The server access settings are read and written by
// this checkout's own access-cli.mjs on a folder that stands for the data
// volume, so what the command prints and saves is what a server would load.
// What this cannot show is real Docker (see the fixture) and systemd.
// Needs bash (Git Bash on Windows); skipped without it.
import test from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync, readFileSync, existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { VERSION } from '../src/restore-cli.mjs';
import { bash, commandMachine } from './docker-command-fixture.mjs';

const skip = !bash && 'no bash';
const lan = 'http://192.0.2.20:18480', secure = 'https://vdi.example.com', publicAddress = 'http://203.0.113.7:18480';
// A server as install.sh leaves it: its own IP, the published port, and the address lines that start it.
const seeded = (http = lan) => ['MULTITAP_SERVER_ADDRESS=192.0.2.20', 'MULTITAP_PUBLISH_PORT=0.0.0.0:18480', 'MULTITAP_PUBLIC_ORIGIN=', 'MULTITAP_HTTP_ORIGIN=' + http, 'MULTITAP_ALLOW_INSECURE_HTTP=' + (http ? 1 : 0)];
const count = (text, pattern) => (text.match(pattern) ?? []).length;
const timer = m => { for (const name of ['multitap-public-ip.service', 'multitap-public-ip.timer']) writeFileSync(join(m.units, name), '[Unit]\n'); };
const units = m => ['multitap-public-ip.service', 'multitap-public-ip.timer'].filter(name => existsSync(join(m.units, name)));

test('status and address print the addresses the server itself has, and say where they are set', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  const shown = m.run(['address']);
  assert.equal(shown.status, 0, shown.out);
  assert.match(shown.out, /Current addresses\n\s+http:\/\/192\.0\.2\.20:18480 \(primary\)\n/); assert.match(shown.out, /To change them: admin console > Server access/); assert.match(shown.out, /When the console cannot be reached: multitap address <url>/);
  assert.equal(existsSync(join(m.volume, 'access')), false, 'reading saves nothing');
  const status = m.run(['status']);
  assert.equal(status.status, 0, status.out);
  assert.match(status.out, new RegExp('Multitap server ' + VERSION.replaceAll('.', '\\.') + ' is running and answering')); assert.match(status.out, /Addresses \(admin console > Server access\)\n\s+http:\/\/192\.0\.2\.20:18480 \(primary\)/);
  assert.match(status.out, /Free disk space: [0-9.]+ GB/); assert.doesNotMatch(status.out, /Disk space is low/);
  assert.match(m.run(['status'], { FAKE_DF_FREE_KB: '1048576' }).out, /Free disk space: 1\.0 GB[^\n]*\n\s+⚠ Disk space is low\. Updates and backups need room\./);

  // No address yet: where the first setup opens, from what the container is told.
  const fresh = commandMachine(t, { version: VERSION, env: seeded('') });
  assert.match(fresh.run(['address']).out, /None yet\. The first setup opens at http:\/\/192\.0\.2\.20:18480/);
  // A server image from before the command: said in plain words, and status still answers.
  const older = m.run(['address'], { FAKE_NO_ACCESS_CLI: VERSION });
  assert.equal(older.status, 1); assert.match(older.out, /does not know this command yet\. Update it first: multitap update/);
  const olderStatus = m.run(['status'], { FAKE_NO_ACCESS_CLI: VERSION });
  assert.equal(olderStatus.status, 0); assert.match(olderStatus.out, /is running and answering/); assert.match(olderStatus.out, /Addresses: Server [0-9.]+ does not know this command yet/);
  assert.equal(m.run(['address', '--help']).status, 0);
});

test('status says what the container is doing when the server does not answer, with the last log lines and never the setup code', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  writeFileSync(join(m.fake, 'logs'), '{"event":"initial_setup_code","setupCode":"ABCD-EFGH-JKLM"}\n{"error":"listen EADDRINUSE"}\n');
  const starting = m.run(['status'], { FAKE_HEALTH: 'starting' });
  assert.match(starting.out, /is starting\. If it does not answer within a minute, check the log below\./); assert.match(starting.out, /listen EADDRINUSE/); assert.doesNotMatch(starting.out, /ABCD-EFGH-JKLM/);
  assert.match(m.run(['status'], { FAKE_HEALTH: 'unhealthy' }).out, /is running but not answering\./);
  writeFileSync(join(m.volume, 'poison'), 'the fake container keeps restarting');
  assert.match(m.run(['status']).out, /Restarts on its own: 2/);
  const restart = m.run(['restart']);
  assert.equal(restart.status, 1); assert.match(restart.out, /Restarted, but the server does not answer\. Log: multitap logs/);
  writeFileSync(join(m.fake, 'state'), 'stopped\n');
  assert.match(m.run(['status']).out, /is stopped\. To start it: multitap start/);
  // The settings name another release than the one that runs.
  const n = commandMachine(t, { version: VERSION, env: seeded() });
  writeFileSync(join(n.fake, 'container-version'), '1.0.0\n');
  assert.match(n.run(['status']).out, /Running release 1\.0\.0; the settings \([^)]*\) say /);
});

test('address <url>... saves the policy the console edits, moves the published port, empties the .env address lines and restarts', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  const set = m.run(['address', secure + '/', 'http://192.0.2.20:8080', 'http://vdi.example.com', '--trusted-proxies', '172.18.0.1']);
  assert.equal(set.status, 0, set.out);
  assert.match(set.out, /Addresses changed\./); assert.match(set.out, /https:\/\/vdi\.example\.com \(primary\)\n\s+http:\/\/192\.0\.2\.20:8080\n\s+http:\/\/vdi\.example\.com\n/); assert.match(set.out, /HTTP is not encrypted/);
  const policy = m.policy().config;
  assert.equal(policy.primaryOrigin, secure); assert.equal(policy.https.origin, secure); assert.equal(policy.http.origin, 'http://192.0.2.20:8080'); assert.deepEqual(policy.http.extraOrigins, ['http://vdi.example.com']); assert.deepEqual(policy.trustedProxies, ['172.18.0.1']);
  // What only this machine can change: the port Docker opens is the one of the address on the server's own IP.
  const env = m.dotenv();
  assert.equal(env.MULTITAP_PUBLISH_PORT, '0.0.0.0:8080'); assert.equal(env.MULTITAP_HTTP_ORIGIN, ''); assert.equal(env.MULTITAP_PUBLIC_ORIGIN, ''); assert.equal(env.MULTITAP_ALLOW_INSECURE_HTTP, '0');
  assert.match(readFileSync(join(m.opt, '.env.previous'), 'utf8'), /^MULTITAP_HTTP_ORIGIN=http:\/\/192\.0\.2\.20:18480$/m, 'the lines as they were are kept beside it');
  assert.equal(count(set.calls, /^compose up -d --force-recreate$/gm), 1); assert.match(m.read('container.env'), /^MULTITAP_PUBLISH_PORT=0\.0\.0\.0:8080$/m);
  assert.doesNotMatch(set.curl, /healthz/);
  // One place: the command shows what was saved, and the same addresses again change nothing.
  const shown = m.run(['address']);
  assert.match(shown.out, /https:\/\/vdi\.example\.com \(primary\)/); assert.match(shown.out, /Trusted proxies: 172\.18\.0\.1/);
  const again = m.run(['address', secure, 'http://192.0.2.20:8080', 'http://vdi.example.com']);
  assert.equal(again.status, 0); assert.match(again.out, /These are already the addresses\./); assert.equal(count(again.calls, /^compose up /gm), 1, 'no restart for nothing');
  assert.deepEqual(m.policy().config.trustedProxies, ['172.18.0.1'], 'the proxies stay unless named');
  const primary = m.run(['address', secure, 'http://192.0.2.20:8080', '--primary', 'http://192.0.2.20:8080/']);
  assert.equal(primary.status, 0, primary.out); assert.equal(m.policy().config.primaryOrigin, 'http://192.0.2.20:8080');
});

test('an HTTPS-only policy: every wait of the command reads the container, never an HTTP address', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  const only = m.run(['address', secure]);
  assert.equal(only.status, 0, only.out);
  assert.equal(m.policy().config.http.enabled, false); assert.equal(m.dotenv().MULTITAP_PUBLISH_PORT, '0.0.0.0:18480', 'the published port is left as it is');
  for (const [args, said] of [[['restart'], /Restarted\./], [['stop'], /Stopped\./], [['start'], /Started\.\n\s+https:\/\/vdi\.example\.com \(primary\)/], [['backup'], /Data backed up: /], [['update', '9.9.9'], /Updated to 9\.9\.9\./]]) {
    const result = m.run(args);
    assert.equal(result.status, 0, args.join(' ') + '\n' + result.out); assert.match(result.out, said);
  }
  const all = m.run(['status']);
  assert.match(all.out, /Multitap server 9\.9\.9 is running and answering/);
  assert.doesNotMatch(all.curl, /healthz|18480/, 'no health address was asked at any point:\n' + all.curl);
  // The release is confirmed by the container's own label: a container of another release is not "updated".
  const wrong = commandMachine(t, { version: VERSION, env: seeded() });
  writeFileSync(join(wrong.fake, 'container-version'), '0.0.1\n');
  const restart = wrong.run(['restart']);
  assert.equal(restart.status, 1, restart.out); assert.match(restart.out, /Restarted, but the server does not answer/);
  // Without an HTTP address nothing can follow the public IP: said plainly.
  const follow = m.run(['public-ip', 'on']);
  assert.equal(follow.status, 1); assert.match(follow.out, /Public IP following needs one other HTTP address/);
});

test('a refused address changes nothing, and a server that does not start with the new addresses gets the previous ones back', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() }), before = readFileSync(join(m.opt, '.env'), 'utf8');
  for (const [args, said] of [[['address', 'vdi.example.com'], /Enter an address that starts with http:\/\/ or https:\/\//], [['address', lan, 'http://192.0.2.20:9090'], /must all use one port\. Docker opens a single port\. The addresses are unchanged\./],
    [['address', lan, '--primary', secure], /The primary address \(--primary\) must be one of the addresses you gave\./], [['address', secure, '--trusted-proxies', '10.0.0.0/8'], /Give the trusted proxies as IP addresses/], [['address', lan, '--bogus'], /Unknown option: --bogus/], [['address', '--primary', lan], /Give at least one address/]]) {
    const result = m.run(args);
    assert.equal(result.status, 1, args.join(' ')); assert.match(result.out, said);
    assert.equal(readFileSync(join(m.opt, '.env'), 'utf8'), before, args.join(' ')); assert.equal(m.policy(), null); assert.doesNotMatch(result.calls, /compose (up|restart|stop)/);
  }
  // Nothing saved before: the way back is "no policy", and the .env lines start the server again.
  const failed = m.run(['address', 'https://broken.example', lan], { FAKE_BROKEN_POLICY: 'broken.example' });
  assert.equal(failed.status, 1, failed.out);
  assert.match(failed.out, /did not start with the new addresses; going back/); assert.match(failed.out, /The previous settings are back and the server is running again\./);
  assert.equal(readFileSync(join(m.opt, '.env'), 'utf8'), before); assert.equal(m.policy(), null); assert.equal(failed.state, 'running');
  assert.match(failed.calls, /access-cli\.mjs revert --force\n/); assert.equal(m.read('container.env'), before, 'the container runs with the settings from before');
  // A saved policy before: exactly that one is back.
  assert.equal(m.run(['address', secure, lan]).status, 0);
  const saved = readFileSync(join(m.volume, 'access', 'policy.json'), 'utf8'), env = readFileSync(join(m.opt, '.env'), 'utf8');
  const second = m.run(['address', 'https://broken.example', 'http://192.0.2.20:7070'], { FAKE_BROKEN_POLICY: 'broken.example' });
  assert.equal(second.status, 1);
  assert.equal(readFileSync(join(m.volume, 'access', 'policy.json'), 'utf8'), saved); assert.equal(readFileSync(join(m.opt, '.env'), 'utf8'), env);
  // Saved settings that can no longer be read: said, with the way out, and --reset takes it.
  writeFileSync(join(m.volume, 'access', 'policy.json'), 'not a policy');
  const unreadable = m.run(['address', lan]);
  assert.equal(unreadable.status, 1); assert.match(unreadable.out, /cannot be read \(server_access_state_invalid\)\. To remove them and set new ones: multitap address --reset <url>/);
  const reset = m.run(['address', '--reset', lan]);
  assert.equal(reset.status, 0, reset.out); assert.equal(m.policy().config.primaryOrigin, lan);
});

test('public-ip on, off and status turn the switch of the saved policy; nothing restarts and no timer is installed', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded(lan + ',' + publicAddress) });
  assert.match(m.run(['public-ip']).out, /Public IP following: off\. To turn it on: admin console > Server access, or multitap public-ip on/);
  // The address is already listed (install.sh lists it for the first start): it is the followed one at once.
  const on = m.run(['public-ip', 'on'], { FAKE_PUBLIC_IP: '203.0.113.7' });
  assert.equal(on.status, 0, on.out);
  assert.match(on.out, /Public IP following is on: http:\/\/203\.0\.113\.7:18480/); assert.match(on.out, /forwards TCP 18480 and UDP 18481/); assert.match(on.out, /not encrypted over the internet/);
  assert.deepEqual(m.policy().publicIp, { follow: true, origin: publicAddress });
  assert.doesNotMatch(on.calls, /compose (up|restart|stop|start)/); assert.equal(on.systemctl, '', 'no unit is written or enabled'); assert.deepEqual(units(m), []);
  assert.match(m.run(['public-ip', 'status']).out, /Public IP following: on \(http:\/\/203\.0\.113\.7:18480\)/);
  assert.match(m.run(['address']).out, /http:\/\/203\.0\.113\.7:18480 \(follows the public IP\)/);
  // The followed address alone cannot be the only one; an HTTPS-only change turns following off with it.
  const alone = m.run(['address', publicAddress]);
  assert.equal(alone.status, 1); assert.match(alone.out, /Public IP following is on, so these addresses cannot be set\./);
  const httpsOnly = m.run(['address', secure]);
  assert.equal(httpsOnly.status, 0, httpsOnly.out); assert.equal(Object.hasOwn(m.policy(), 'publicIp'), false);

  // The lookup gives nothing: following is on and the server registers the address itself.
  const n = commandMachine(t, { version: VERSION, env: seeded() });
  const blind = n.run(['public-ip', 'on']);
  assert.equal(blind.status, 0, blind.out); assert.match(blind.out, /The server looks up its public IP now and registers the address, usually within a minute\./); assert.match(blind.out, /open connections end once/); assert.doesNotMatch(blind.out, /within 10 minutes/);
  assert.deepEqual(n.policy().publicIp, { follow: true, origin: null });
  const waiting = n.run(['public-ip']).out;
  assert.match(waiting, /Public IP following: on \(no address registered yet\)\n\s+The server registers it as soon as it finds its public IP\. When it cannot, it tries again every 10 minutes\./); assert.doesNotMatch(waiting, /within 10 minutes/);
  const off = n.run(['public-ip', 'off']);
  assert.equal(off.status, 0); assert.match(off.out, /Public IP following is off\. The registered address stays\./); assert.equal(Object.hasOwn(n.policy(), 'publicIp'), false);
  assert.equal(n.run(['public-ip', 'sideways']).status, 1);
});

test('update moves the public IP timer of release 1.1.1 into the server, keeps three pre-update copies and the two newest images', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded(lan + ',' + publicAddress) });
  timer(m); writeFileSync(join(m.opt, 'public-ip'), '203.0.113.7\n');
  writeFileSync(join(m.fake, 'images'), ['0.9.0', '1.0.0', VERSION, '9.9.9', 'latest', ''].join('\n'));
  const kept = ['multitap-data-1.0.0-20250101-010101.tar.gz', 'multitap-data-1.0.0-20250102-010101-before-restore.tar.gz', 'not-ours-before-update.tar.gz'];
  // The time in the name decides, not the release before it.
  const copies = ['multitap-data-1.0.9-20250201-010101-before-update.tar.gz', 'multitap-data-1.0.10-20250301-010101-before-update.tar.gz', 'multitap-data-1.0.2-20250401-010101-before-update.tar.gz', 'multitap-data-1.0.3-20250501-010101-before-update.tar.gz'];
  for (const name of [...kept, ...copies]) writeFileSync(join(m.opt, 'backups', name), 'x');
  writeFileSync(join(m.volume, 'vault.key'), 'k');
  const updated = m.run(['update', '9.9.9']);
  assert.equal(updated.status, 0, updated.out);
  assert.match(updated.out, /Updated to 9\.9\.9\. Your data is unchanged\./);
  // Backups: the new copy and the two newest earlier ones stay; copies made by hand and safety copies are never touched.
  assert.match(updated.out, /Removed an old pre-update backup: multitap-data-1\.0\.9-20250201-010101-before-update\.tar\.gz/); assert.match(updated.out, /Removed an old pre-update backup: multitap-data-1\.0\.10-20250301-010101-before-update\.tar\.gz/);
  const left = m.backups(), fresh = left.filter(name => !kept.includes(name) && !copies.includes(name));
  assert.equal(fresh.length, 1); assert.match(fresh[0], new RegExp('^multitap-data-' + VERSION.replaceAll('.', '\\.') + '-\\d{8}-\\d{6}-before-update\\.tar\\.gz$'));
  assert.deepEqual(left.filter(name => name !== fresh[0]).sort(), [...kept, copies[2], copies[3]].sort());
  // Images: the release now and the one before stay; older ones go; other tags are not ours to remove.
  assert.match(updated.out, /Removed an old server image: 0\.9\.0/); assert.match(updated.out, /Removed an old server image: 1\.0\.0/);
  assert.deepEqual(m.read('images').split('\n').filter(Boolean), [VERSION, '9.9.9', 'latest']);
  // The timer: following is on in the saved policy with the address it had registered, then it is gone. One start only.
  assert.match(updated.out, /Public IP following is now done by the server itself\. The earlier 10-minute timer is removed\./);
  assert.deepEqual(m.policy().publicIp, { follow: true, origin: publicAddress }); assert.deepEqual(units(m), []); assert.equal(existsSync(join(m.opt, 'public-ip')), false);
  assert.match(updated.systemctl, /^disable --now multitap-public-ip\.timer$/m); assert.match(updated.calls, /access-cli\.mjs public-ip on --address 203\.0\.113\.7\n/);
  assert.equal(count(updated.calls, /^compose (up|restart|start) /gm), 1, 'the server is started once, with the new release:\n' + updated.calls);
  assert.doesNotMatch(updated.calls, /^compose start/m, 'the copy before an update does not start the old release again');
  assert.doesNotMatch(updated.curl, /healthz/);
});

test('the timer itself finishes the move after an update made by the 1.1.1 command; a server that cannot follow is told why', { skip }, t => {
  // What the 1.1.1 timer runs every ten minutes is this file's `public-ip sync` once the command was replaced.
  const m = commandMachine(t, { version: VERSION, env: seeded(lan + ',' + publicAddress) });
  timer(m); writeFileSync(join(m.opt, 'public-ip'), '203.0.113.7\n');
  // The server is still the old release (the update is under way, or was rolled back): nothing changes, nothing is said.
  const early = m.run(['public-ip', 'sync'], { FAKE_NO_ACCESS_CLI: VERSION });
  assert.equal(early.status, 0, early.out); assert.equal(early.out, ''); assert.equal(units(m).length, 2); assert.equal(m.policy(), null);
  const moved = m.run(['public-ip', 'sync']);
  assert.equal(moved.status, 0, moved.out); assert.match(moved.out, /Public IP following is now done by the server itself/);
  assert.deepEqual(m.policy().publicIp, { follow: true, origin: publicAddress }); assert.deepEqual(units(m), []);
  assert.doesNotMatch(moved.calls, /compose (up|restart|stop|start)/, 'nobody is disconnected for it');
  assert.equal(m.run(['public-ip', 'sync']).out, '', 'without the timer there is nothing to do');

  // HTTPS only: following cannot work. The timer stays, the tick says nothing, a person at the terminal is told.
  const n = commandMachine(t, { version: VERSION, env: ['MULTITAP_SERVER_ADDRESS=192.0.2.20', 'MULTITAP_PUBLISH_PORT=0.0.0.0:18480', 'MULTITAP_PUBLIC_ORIGIN=' + secure] });
  timer(n); writeFileSync(join(n.opt, 'public-ip'), '203.0.113.7\n');
  assert.equal(n.run(['public-ip', 'sync']).out, '');
  const told = n.run(['update', '9.9.9']);
  assert.equal(told.status, 0, told.out);
  assert.match(told.out, /Public IP following could not be moved into the server\. It cannot work with the current addresses\./); assert.match(told.out, /add an HTTP address and turn on “Follow the public IP”\. To stop using it: multitap public-ip off/);
  assert.equal(units(n).length, 2); assert.equal(n.policy(), null);
  const off = n.run(['public-ip', 'off']);
  assert.equal(off.status, 0, off.out); assert.deepEqual(units(n), []); assert.equal(existsSync(join(n.opt, 'public-ip')), false);
});

test('update takes the new release\'s compose.yaml, empties address lines a saved policy has superseded, and puts every file back when the release does not answer', { skip }, async t => {
  const repository = name => readFileSync(fileURLToPath(new URL('../../' + name, import.meta.url)), 'utf8').replaceAll('\r\n', '\n');
  // The server is told the published UDP port by both compose files; without the line no UDP address is derived.
  for (const name of ['compose.yaml', 'compose.direct-https.yaml']) assert.ok(repository(name).includes('      MULTITAP_UDP_PUBLISHED_PORT: ${MULTITAP_UDP_PUBLISH_PORT:-18481}\n'), name);
  const m = commandMachine(t, { version: VERSION, env: seeded(lan + ',' + publicAddress) });
  writeFileSync(join(m.fake, 'release-compose.yaml'), repository('compose.yaml'));
  // A policy is saved (here by public-ip on) while the .env lines are still there.
  assert.equal(m.run(['public-ip', 'on'], { FAKE_PUBLIC_IP: '203.0.113.7' }).status, 0);
  const before = readFileSync(join(m.opt, '.env'), 'utf8');
  assert.match(before, /^MULTITAP_HTTP_ORIGIN=http:/m);
  const failed = m.run(['update', '9.9.9'], { MULTITAP_LOCAL: '', FAKE_BROKEN_RELEASE: '9.9.9' });
  assert.equal(failed.status, 1, failed.out);
  assert.match(failed.out, /9\.9\.9 did not start\. Went back to [0-9.]+ and the server is running again\./);
  assert.equal(readFileSync(join(m.opt, '.env'), 'utf8'), before); assert.equal(readFileSync(join(m.opt, 'compose.yaml'), 'utf8'), 'name: multitap\n'); assert.equal(m.read('container-version').trim(), VERSION);
  assert.equal(failed.state, 'running');

  const updated = m.run(['update', '9.9.9'], { MULTITAP_LOCAL: '' });
  assert.equal(updated.status, 0, updated.out);
  assert.match(updated.curl, /https:\/\/raw\.githubusercontent\.com\/totblabs\/Multitap\/v9\.9\.9\/compose\.yaml/);
  assert.equal(readFileSync(join(m.opt, 'compose.yaml'), 'utf8'), repository('compose.yaml')); assert.equal(readFileSync(join(m.opt, 'compose.yaml.previous'), 'utf8'), 'name: multitap\n');
  assert.match(m.read('container.compose.yaml'), /MULTITAP_UDP_PUBLISHED_PORT: \$\{MULTITAP_UDP_PUBLISH_PORT:-18481\}/, 'the container of the new release is created from the new file');
  const env = m.dotenv();
  assert.equal(env.MULTITAP_VERSION, '9.9.9'); assert.equal(env.MULTITAP_HTTP_ORIGIN, ''); assert.equal(env.MULTITAP_ALLOW_INSECURE_HTTP, '0'); assert.equal(env.MULTITAP_PUBLISH_PORT, '0.0.0.0:18480');
  assert.match(readFileSync(join(m.opt, '.env.previous'), 'utf8'), /^MULTITAP_HTTP_ORIGIN=http:/m);
  assert.match(m.run(['address']).out, /http:\/\/192\.0\.2\.20:18480 \(primary\)\n\s+http:\/\/203\.0\.113\.7:18480 \(follows the public IP\)/, 'the addresses are the saved ones, as before');

  // A server that has saved nothing keeps the lines that start it.
  const n = commandMachine(t, { version: VERSION, env: seeded() });
  assert.equal(n.run(['update', '9.9.9']).status, 0);
  assert.equal(n.dotenv().MULTITAP_HTTP_ORIGIN, lan); assert.equal(n.policy(), null);
  mkdirSync(join(n.opt, 'nothing'), { recursive: true });
});

test('going back to a release from before the saved policy gives it the saved addresses as its .env lines; the same release again removes no image', { skip }, t => {
  // Seen on a real install: `update 1.1.1` from 1.1.2 left the emptied lines, and 1.1.1 started with no address at all.
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  assert.equal(m.run(['address', secure, lan, '--trusted-proxies', '172.18.0.1']).status, 0);
  assert.equal(m.dotenv().MULTITAP_HTTP_ORIGIN, '', 'the saved policy has superseded the lines');
  writeFileSync(join(m.fake, 'images'), ['1.0.9', VERSION, ''].join('\n'));
  const back = m.run(['update', '1.0.9'], { FAKE_NO_ACCESS_CLI: '1.0.9' });
  assert.equal(back.status, 0, back.out);
  const env = m.dotenv();
  assert.equal(env.MULTITAP_VERSION, '1.0.9'); assert.equal(env.MULTITAP_PUBLIC_ORIGIN, secure); assert.equal(env.MULTITAP_HTTP_ORIGIN, lan); assert.equal(env.MULTITAP_ALLOW_INSECURE_HTTP, '1'); assert.equal(env.MULTITAP_TRUSTED_PROXIES, '172.18.0.1');
  assert.match(m.read('container.env'), /^MULTITAP_HTTP_ORIGIN=http:\/\/192\.0\.2\.20:18480$/m, 'the older server is created with the lines');
  assert.equal(m.policy().config.primaryOrigin, secure, 'the saved policy stays for the way forward');

  // HTTP only: no HTTPS line and no proxies line.
  const n = commandMachine(t, { version: VERSION, env: seeded() });
  assert.equal(n.run(['address', lan, publicAddress]).status, 0);
  assert.equal(n.run(['update', '1.0.9'], { FAKE_NO_ACCESS_CLI: '1.0.9' }).status, 0);
  assert.equal(n.dotenv().MULTITAP_PUBLIC_ORIGIN, ''); assert.equal(n.dotenv().MULTITAP_HTTP_ORIGIN, lan + ',' + publicAddress); assert.equal(Object.hasOwn(n.dotenv(), 'MULTITAP_TRUSTED_PROXIES'), false);

  // `update <the release it runs>` on a stopped server starts it; the release before is not known, so no image goes.
  const s = commandMachine(t, { version: VERSION, env: seeded() });
  writeFileSync(join(s.fake, 'images'), ['1.0.0', VERSION, ''].join('\n'));
  assert.equal(s.run(['stop']).status, 0);
  const again = s.run(['update', VERSION]);
  assert.equal(again.status, 0, again.out); assert.equal(again.state, 'running');
  assert.doesNotMatch(again.out, /Removed an old server image/); assert.deepEqual(s.read('images').split('\n').filter(Boolean), ['1.0.0', VERSION]);
});

test('saved access settings the server could not use: status, address and every wait say that it runs without addresses, where the console opens and the way out', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  assert.equal(m.run(['address', lan, publicAddress]).status, 0);
  // The container stays healthy: such a server starts without addresses instead of ending.
  writeFileSync(join(m.volume, 'access', 'policy.json'), '{not a policy');
  const said = [/The saved server access settings could not be read \(server_access_state_invalid\)\./, /The server is running without addresses\. Apps and Hosts cannot connect\./,
    /The admin console is open at http:\/\/192\.0\.2\.20:18480\. Sign in there and save the addresses again in Server access\./, /To set them from here: multitap address --reset <url>\.\.\./];
  const status = m.run(['status']);
  assert.equal(status.status, 0, status.out); assert.match(status.out, /is running and answering/); for (const line of said) assert.match(status.out, line);
  const shown = m.run(['address']);
  assert.equal(shown.status, 1, shown.out); for (const line of said) assert.match(shown.out, line);
  for (const [args, done] of [[['restart'], /Restarted\./], [['start'], /Started\./], [['update', '9.9.9'], /Updated to 9\.9\.9\./]]) {
    const result = m.run(args);
    assert.equal(result.status, 0, args.join(' ') + '\n' + result.out); assert.match(result.out, done); for (const line of said) assert.match(result.out, line, args.join(' '));
  }
  // Where the console does not open that way it is not named: the port published on this machine only, or no server address.
  const env = readFileSync(join(m.opt, '.env'), 'utf8');
  assert.match(env, /^MULTITAP_PUBLISH_PORT=0\.0\.0\.0:18480$/m); assert.match(env, /^MULTITAP_SERVER_ADDRESS=192\.0\.2\.20$/m);
  writeFileSync(join(m.opt, '.env'), env.replace('MULTITAP_PUBLISH_PORT=0.0.0.0:18480', 'MULTITAP_PUBLISH_PORT=127.0.0.1:18480'));
  const local = m.run(['status']).out;
  assert.match(local, said[0]); assert.match(local, said[3]); assert.doesNotMatch(local, /admin console is open at/);
  writeFileSync(join(m.opt, '.env'), env.replace('MULTITAP_SERVER_ADDRESS=192.0.2.20', 'MULTITAP_SERVER_ADDRESS='));
  assert.doesNotMatch(m.run(['status']).out, /admin console is open at/);
  writeFileSync(join(m.opt, '.env'), env);
  const reset = m.run(['address', '--reset', lan]);
  assert.equal(reset.status, 0, reset.out); assert.equal(m.policy().config.primaryOrigin, lan);
  const after = m.run(['restart']);
  assert.equal(after.status, 0); assert.doesNotMatch(after.out, /could not be read/); assert.doesNotMatch(m.run(['status']).out, /could not be read/);
});

test('a policy file the server\'s account may not read: the command says the server cannot start, and --reset removes the file as root', { skip }, t => {
  const m = commandMachine(t, { version: VERSION, env: seeded() });
  assert.equal(m.run(['address', lan, publicAddress]).status, 0);
  // The fake's stand-in for a file of another owner (fake-access-cli.mjs): the server's own reset cannot read it either.
  writeFileSync(join(m.volume, 'access', 'policy.json'), '#another-owner\n' + readFileSync(join(m.volume, 'access', 'policy.json'), 'utf8'));
  const status = m.run(['status']);
  assert.match(status.out, /The saved server access settings could not be read \(server_access_storage_invalid\)\./); assert.match(status.out, /The server's account cannot read the file, so the server cannot start\./);
  assert.match(status.out, /To remove it and set the addresses again: multitap address --reset <url>\.\.\./); assert.doesNotMatch(status.out, /admin console is open at/);
  const refused = m.run(['address', lan]);
  assert.equal(refused.status, 1); assert.match(refused.out, /cannot be read \(server_access_storage_invalid\)\. To remove them and set new ones: multitap address --reset <url>/);
  const reset = m.run(['address', '--reset', lan]);
  assert.equal(reset.status, 0, reset.out);
  assert.match(reset.calls, /access-cli\.mjs reset\n/); assert.match(reset.calls, /^run --rm --network none --user 0 --entrypoint sh .* rm -f \/data\/access\/policy\.json /m, 'root removes what the server\'s account could not');
  assert.equal(m.policy().config.primaryOrigin, lan); assert.equal(m.policy().config.http.extraOrigins, undefined);
  assert.match(m.run(['address']).out, /http:\/\/192\.0\.2\.20:18480 \(primary\)/);
});

test('address <url>... while the public IP is followed keeps the followed address in the list, so the server is started once', { skip }, t => {
  const other = 'http://multitap.lan.example:18480';
  const m = commandMachine(t, { version: VERSION, env: seeded(lan + ',' + publicAddress) });
  assert.equal(m.run(['public-ip', 'on'], { FAKE_PUBLIC_IP: '203.0.113.7' }).status, 0);
  const changed = m.run(['address', lan, other]);
  assert.equal(changed.status, 0, changed.out);
  assert.match(changed.calls, /access-cli\.mjs set http:\/\/192\.0\.2\.20:18480 http:\/\/multitap\.lan\.example:18480 http:\/\/203\.0\.113\.7:18480\n/);
  assert.match(changed.out, /http:\/\/multitap\.lan\.example:18480\n\s+http:\/\/203\.0\.113\.7:18480 \(follows the public IP\)/);
  assert.deepEqual(m.policy().config.http.extraOrigins, [other, publicAddress]); assert.deepEqual(m.policy().publicIp, { follow: true, origin: publicAddress });
  // Another port: the public address moves to it with the published port; the server marks it as followed at its next round.
  const moved = m.run(['address', 'http://192.0.2.20:8080']);
  assert.equal(moved.status, 0, moved.out); assert.equal(m.dotenv().MULTITAP_PUBLISH_PORT, '0.0.0.0:8080');
  assert.deepEqual(m.policy().config.http.extraOrigins, ['http://203.0.113.7:8080']); assert.equal(m.policy().publicIp.follow, true);
  // Four HTTP addresses given: there is no room, the list is exactly what was asked for.
  const four = ['http://192.0.2.20:8080', 'http://a.lan.example:8080', 'http://b.lan.example:8080', 'http://c.lan.example:8080'];
  const full = m.run(['address', ...four]);
  assert.equal(full.status, 0, full.out); assert.deepEqual([m.policy().config.http.origin, ...m.policy().config.http.extraOrigins], four);
  // Following off: nothing is added.
  assert.equal(m.run(['public-ip', 'off']).status, 0);
  assert.equal(m.run(['address', 'http://192.0.2.20:8080', 'http://a.lan.example:8080']).status, 0);
  assert.deepEqual(m.policy().config.http.extraOrigins, ['http://a.lan.example:8080']);
});
