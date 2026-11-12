// src/access-cli.mjs: the server access settings of a Docker Compose install
// from the command line, on a temporary data folder with the real
// configuration loader. No Docker: the container this runs in is its
// environment and that folder.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile, writeFile, readdir, mkdir, rename } from 'node:fs/promises';
import { readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';
import { main } from '../src/access-cli.mjs';
import { configuration, selfAccessStorage } from '../src/config.mjs';

const lan = 'http://192.0.2.10:18480', secure = 'https://vdi.example.com', publicAddress = 'http://203.0.113.7:18480';
const readAccessFile = path => { try { return readFileSync(path, 'utf8'); } catch (error) { if (error.code === 'ENOENT') return null; throw error; } };
async function install(t, extra = {}) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-access-cli-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const env = { MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '18480', MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_PUBLISHED_PORT: '0.0.0.0:18480', MULTITAP_SERVER_ADDRESS: '192.0.2.10',
    MULTITAP_HTTP_ORIGIN: lan, MULTITAP_ALLOW_INSECURE_HTTP: '1', ...extra };
  const run = async (...argv) => {
    let text = '';
    const code = await main(argv, { env, stdout: { write: chunk => { text += chunk; } }, configure: value => configuration(value, { readAccessFile }), storage: value => selfAccessStorage(value, { readAccessFile }) });
    return { code, text, values: Object.fromEntries(text.split('\n').filter(Boolean).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)])) };
  };
  return { directory, env, run, load: () => configuration(env, { readAccessFile }), files: () => readdir(join(directory, 'access')).then(list => list.sort(), () => []) };
}

test('show prints the addresses in use and what the container fixes; set replaces them in the volume and revert puts the earlier ones back', async t => {
  const f = await install(t);
  const shown = await f.run('show');
  assert.equal(shown.code, 0);
  assert.deepEqual(shown.values, { ok: '1', source: 'env', revision: f.load().accessRevision, primary: lan, https: '', http: lan, trustedProxies: '127.0.0.1,::1', follow: '0', followed: '', followOffered: '1',
    tlsMode: 'proxy', publishedHost: '0.0.0.0', publishedPort: '18480', serverAddress: '192.0.2.10', pending: '', envAddressesIgnored: '0' });
  assert.deepEqual(await f.files(), [], 'reading writes nothing');

  // The HTTPS address is the primary one unless another is named, as the .env lines have it.
  const set = await f.run('set', lan + '/', secure, 'http://vdi.example.com:18480');
  assert.equal(set.code, 0);
  assert.equal(set.values.ok, '1'); assert.equal(set.values.changed, '1'); assert.equal(set.values.source, 'saved'); assert.equal(set.values.primary, secure);
  assert.equal(set.values.https, secure); assert.equal(set.values.http, lan + ',http://vdi.example.com:18480');
  // Saved beside the .env lines of now: they are its seed, not an edit somebody expects to work.
  assert.equal(set.values.envAddressesIgnored, '0');
  f.env.MULTITAP_HTTP_ORIGIN = 'http://192.0.2.50:18480';
  assert.equal((await f.run('show')).values.envAddressesIgnored, '1', 'edited after the save');
  assert.equal((await f.run('public-ip', 'status')).values.envAddressesIgnored, '1');
  f.env.MULTITAP_HTTP_ORIGIN = ''; f.env.MULTITAP_ALLOW_INSECURE_HTTP = '0';
  assert.equal((await f.run('show')).values.envAddressesIgnored, '0', 'emptied lines are nothing to ignore');
  f.env.MULTITAP_HTTP_ORIGIN = lan; f.env.MULTITAP_ALLOW_INSECURE_HTTP = '1';
  assert.deepEqual(await f.files(), ['identity', 'policy.before-cli.json', 'policy.json']);
  assert.deepEqual(await readdir(f.directory), ['access'], 'never the file names an earlier server reads in the data folder');
  const loaded = f.load();
  assert.deepEqual(loaded.allowedOrigins, [secure, lan, 'http://vdi.example.com:18480']); assert.equal(loaded.accessConfig.https.listenPort, 18480); assert.equal(loaded.accessConfig.http.listenPort, 18480);
  assert.match(loaded.accessInstallationId, /^[a-f0-9]{64}$/);

  const chosen = await f.run('set', lan, secure, '--primary', lan, '--trusted-proxies', '172.18.0.1');
  assert.equal(chosen.values.primary, lan); assert.equal(chosen.values.trustedProxies, '172.18.0.1'); assert.equal(chosen.values.changed, '1');
  assert.equal((await f.run('set', lan, secure, '--primary', lan)).values.changed, '0', 'the same addresses: nothing to restart for; the proxies stay');
  assert.equal(f.load().accessConfig.trustedProxies.join(), '172.18.0.1');
  assert.equal((await f.run('set', lan, '--trusted-proxies', 'none')).values.trustedProxies, '');

  // revert: what the last change replaced.
  const reverted = await f.run('revert');
  assert.equal(reverted.values.ok, '1'); assert.equal(reverted.values.http, lan); assert.equal(reverted.values.https, secure); assert.equal(reverted.values.trustedProxies, '172.18.0.1');
  assert.deepEqual(await f.run('revert').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_revert_unavailable\n']);
});

test('set on an install that only has its .env lines saves them as the policy, and a refused address changes nothing', async t => {
  const f = await install(t);
  const same = await f.run('set', lan);
  assert.equal(same.values.changed, '0'); assert.equal(same.values.source, 'saved'); assert.equal(same.values.revision, f.load().accessRevision);
  const before = await readFile(join(f.directory, 'access', 'policy.json'), 'utf8');
  for (const [argv, code] of [
    [['set', 'ftp://192.0.2.10'], 'server_access_origin_invalid'], [['set', lan, lan], 'server_access_origin_invalid'],
    [['set', lan, '--primary', secure], 'server_access_primary_origin_invalid'], [['set', lan, '--trusted-proxies', '10.0.0.0/8'], 'server_access_trusted_proxies_invalid'],
    // The server's own address answers on the published port only.
    [['set', lan, 'http://192.0.2.10:8080'], 'server_access_published_port_conflict'],
    [['set', lan, 'http://a.example.com', 'http://b.example.com', 'http://c.example.com', 'http://d.example.com'], 'server_access_origin_invalid'],
  ]) assert.deepEqual(await f.run(...argv).then(result => [result.code, result.text]), [2, 'ok=0\ncode=' + code + '\n'], argv.join(' '));
  assert.equal(await readFile(join(f.directory, 'access', 'policy.json'), 'utf8'), before);
  for (const argv of [[], ['set'], ['show', 'more'], ['set', lan, '--follow', 'maybe'], ['public-ip'], ['public-ip', 'sync'], ['public-ip', 'off', '--address', '203.0.113.7'], ['reset', '--force'], ['set', lan, '--unknown']]) {
    const result = await f.run(...argv);
    assert.equal(result.code, 64, argv.join(' ')); assert.match(result.text, /^usage: access-cli\.mjs show/);
  }
});

test('a save from the console that is still in flight is not written over, unless forced; reset removes even a policy that does not load', async t => {
  const f = await install(t);
  await f.run('set', lan, secure);
  const revision = f.load().accessRevision;
  await writeFile(join(f.directory, 'access', 'journal.json'), JSON.stringify({ schema: 1, operationId: randomUUID(), state: 'health_check', revision, previousRevision: 'a'.repeat(64), previous: null }));
  assert.equal((await f.run('show')).values.pending, 'health_check');
  for (const argv of [['set', lan], ['public-ip', 'on'], ['revert']]) assert.deepEqual(await f.run(...argv).then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_busy\n'], argv.join(' '));
  const forced = await f.run('set', lan, '--force');
  assert.equal(forced.values.ok, '1'); assert.equal(forced.values.pending, ''); assert.equal(forced.values.http, lan);
  assert.equal((await f.files()).includes('journal.json'), false, 'the save it replaced is over');

  await writeFile(join(f.directory, 'access', 'policy.json'), 'not a policy');
  assert.deepEqual(await f.run('show').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_state_invalid\n']);
  const reset = await f.run('reset');
  assert.equal(reset.values.ok, '1'); assert.equal(reset.values.changed, '1'); assert.equal(reset.values.source, 'env'); assert.equal(reset.values.http, lan);
  assert.deepEqual(await f.files(), ['identity']);
  assert.equal((await f.run('reset')).values.changed, '0');
});

test('public-ip on, off and status set and read the switch of the saved policy; an address already registered is taken over', async t => {
  const f = await install(t, { MULTITAP_HTTP_ORIGIN: lan + ',' + publicAddress });
  const status = await f.run('public-ip', 'status');
  assert.equal(status.values.follow, '0'); assert.equal(status.values.followOffered, '1'); assert.equal(Object.hasOwn(status.values, 'changed'), false);
  assert.equal((await f.run('public-ip', 'off')).values.changed, '0'); assert.deepEqual(await f.files(), [], 'off where nothing is followed writes nothing');

  // The host's earlier timer had registered this address: it is the followed one from now on.
  const on = await f.run('public-ip', 'on', '--address', '203.0.113.7');
  assert.equal(on.code, 0); assert.equal(on.values.changed, '1'); assert.equal(on.values.follow, '1'); assert.equal(on.values.followed, publicAddress); assert.equal(on.values.http, lan + ',' + publicAddress); assert.equal(on.values.source, 'saved');
  const loaded = f.load();
  assert.deepEqual(loaded.selfAccess.publicIp, { follow: true, origin: publicAddress }); assert.deepEqual(loaded.allowedOrigins, [lan, publicAddress]);
  assert.notEqual(loaded.selfAccess.revision, loaded.accessRevision);
  assert.equal((await f.run('public-ip', 'on', '--address', '203.0.113.7')).values.changed, '0');
  // Other addresses by command keep following and its address, when they still list it.
  const kept = await f.run('set', lan, 'http://vdi.example.com:18480', publicAddress);
  assert.equal(kept.values.follow, '1'); assert.equal(kept.values.followed, publicAddress); assert.equal(kept.values.http, lan + ',http://vdi.example.com:18480,' + publicAddress);
  const dropped = await f.run('set', lan);
  assert.equal(dropped.values.follow, '1'); assert.equal(dropped.values.followed, '', 'the server registers it again at its next round');
  // Without an HTTP address there is nothing to follow with: said, or turned off in the same command.
  assert.deepEqual(await f.run('set', secure).then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_public_ip_conflict\n']);
  const secured = await f.run('set', secure, '--follow', 'off');
  assert.equal(secured.values.follow, '0'); assert.equal(secured.values.http, ''); assert.equal(secured.values.followOffered, '0');
  assert.deepEqual(await f.run('public-ip', 'on').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_public_ip_conflict\n']);

  // off: the registered address stays as an ordinary one.
  await f.run('set', lan, publicAddress, '--follow', 'on');
  await f.run('public-ip', 'on', '--address', '203.0.113.7');
  const off = await f.run('public-ip', 'off');
  assert.equal(off.values.changed, '1'); assert.equal(off.values.follow, '0'); assert.equal(off.values.followed, ''); assert.equal(off.values.http, lan + ',' + publicAddress);
  assert.equal(Object.hasOwn(JSON.parse(await readFile(join(f.directory, 'access', 'policy.json'), 'utf8')), 'publicIp'), false);
  // An address that is not registered, or no public address at all.
  const unknown = await f.run('public-ip', 'on', '--address', '198.51.100.20');
  assert.equal(unknown.values.follow, '1'); assert.equal(unknown.values.followed, '');
  assert.deepEqual(await f.run('public-ip', 'on', '--address', '192.168.0.10').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_public_ip_invalid\n']);

  // Published on loopback only, or the only HTTP address is the public one: not there.
  const loopback = await install(t, { MULTITAP_PUBLISHED_PORT: '127.0.0.1:18480' });
  assert.equal((await loopback.run('public-ip', 'status')).values.followOffered, '0');
  assert.deepEqual(await loopback.run('public-ip', 'on').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_public_ip_conflict\n']);
  const only = await install(t, { MULTITAP_HTTP_ORIGIN: publicAddress });
  assert.deepEqual(await only.run('public-ip', 'on', '--address', '203.0.113.7').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_public_ip_conflict\n']);
});

test('an install whose settings an updater applies, or that is no Docker install, is not this command\'s', async t => {
  for (const extra of [{ MULTITAP_UPDATE_SOCKET: '/run/multitap-docker-update/control.sock' }, { MULTITAP_DEPLOYMENT_KIND: 'systemd' }]) {
    const f = await install(t, { MULTITAP_PUBLIC_ORIGIN: secure, MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0', MULTITAP_SERVER_ADDRESS: '', ...extra });
    for (const argv of [['show'], ['set', secure], ['public-ip', 'on'], ['revert'], ['reset']]) assert.deepEqual(await f.run(...argv).then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_unavailable\n'], argv.join(' '));
    await mkdir(join(f.directory, 'access'), { recursive: true });
    assert.deepEqual(await f.files(), []);
  }
});

test('reset removes a policy its account cannot even read, and says so when it may not remove files in the folder', async t => {
  const f = await install(t);
  await f.run('set', lan, secure);
  const identity = await readFile(join(f.directory, 'access', 'identity'), 'utf8');
  // Written by another account (the command line run as root): the server's reader refuses the files unread.
  const foreign = new Set(['policy.json']);
  const refusing = path => { if (foreign.has(path.replaceAll('\\', '/').split('/').at(-1)) && existsSync(path)) throw Error('server_access_storage_invalid'); return readAccessFile(path); };
  const run = async (...argv) => {
    let text = '';
    const code = await main(argv, { env: f.env, stdout: { write: chunk => { text += chunk; } }, configure: value => configuration(value, { readAccessFile: refusing }), storage: value => selfAccessStorage(value, { readAccessFile: refusing }) });
    return { code, text, values: Object.fromEntries(text.split('\n').filter(Boolean).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)])) };
  };
  assert.deepEqual(await run('show').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_storage_invalid\n']);
  const reset = await run('reset');
  assert.equal(reset.code, 0, reset.text); assert.equal(reset.values.ok, '1'); assert.equal(reset.values.changed, '1'); assert.equal(reset.values.source, 'env'); assert.equal(reset.values.http, lan);
  assert.deepEqual(await f.files(), ['identity'], 'the installation ID it can read stays');
  assert.equal(await readFile(join(f.directory, 'access', 'identity'), 'utf8'), identity);
  // An installation ID it cannot read either goes too: no server starts with such a file.
  await f.run('set', lan, secure); foreign.add('identity');
  const both = await run('reset');
  assert.equal(both.code, 0, both.text); assert.equal(both.values.changed, '1');
  assert.deepEqual(await f.files(), []);
  foreign.clear();
  // Where it may not remove the file (here: a folder stands in its place), a code of its own, and nothing else is touched.
  await f.run('set', lan, secure);
  const policy = join(f.directory, 'access', 'policy.json'), kept = await readFile(policy, 'utf8');
  await rename(policy, policy + '.moved'); await mkdir(join(policy, 'inside'), { recursive: true });
  assert.deepEqual(await run('reset').then(result => [result.code, result.text]), [2, 'ok=0\ncode=server_access_reset_denied\n']);
  await rm(policy, { recursive: true }); await rename(policy + '.moved', policy);
  assert.equal(await readFile(policy, 'utf8'), kept);
  assert.equal((await run('reset')).values.changed, '1'); assert.equal((await run('reset')).values.changed, '0');
});
