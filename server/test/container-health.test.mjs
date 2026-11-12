// The container's HEALTHCHECK command (update/server-access-health.mjs
// --container-health), run as the image runs it: its own process, against the
// real server entry (src/index.mjs) on a temporary data folder configured as a
// Docker Compose install. It loads the server's configuration, whose modules
// import the health module back: the command must still end, with 0 while the
// server answers and 1 when it does not. No container and no Docker.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:http';
import { fileURLToPath } from 'node:url';

const entry = fileURLToPath(new URL('../src/index.mjs', import.meta.url)), health = fileURLToPath(new URL('../update/server-access-health.mjs', import.meta.url));
const freePort = () => new Promise(resolve => { const probe = createServer(); probe.listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close(() => resolve(port)); }); });
const ended = child => new Promise(resolve => child.once('exit', (code, signal) => resolve(signal ?? code)));

test('the container health command ends with 0 while the server answers and 1 when it does not', { timeout: 60000 }, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-container-health-')), port = await freePort();
  const env = { PATH: process.env.PATH, SystemRoot: process.env.SystemRoot, MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port),
    MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:' + port, MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TLS_MODE: 'proxy', MULTITAP_DOCKER_UPDATE_FEED: 'off' };
  const check = async () => {
    const child = spawn(process.execPath, [health, '--container-health'], { env, stdio: 'ignore' });
    const timer = setTimeout(() => child.kill(), 20000);
    try { return await ended(child); } finally { clearTimeout(timer); }
  };
  // Nothing listens yet.
  assert.equal(await check(), 1);
  const server = spawn(process.execPath, [entry], { env, stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  server.stdout.on('data', chunk => { output += chunk; }); server.stderr.on('data', chunk => { output += chunk; });
  const stopped = ended(server);
  t.after(async () => { server.kill(); await stopped; await rm(directory, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); });
  for (const end = Date.now() + 30000; !output.includes('"listeners"');) { assert.ok(Date.now() < end && server.exitCode === null, 'the server did not start:\n' + output); await new Promise(resolve => setTimeout(resolve, 50)); }
  assert.equal(await check(), 0, 'not 13: the command must not wait on its own import');
  server.kill(); await stopped;
  assert.equal(await check(), 1);
});

// The reported sequence with the real processes. Linux and macOS only: the
// server's reader of its own files asks for an owner and a mode a Windows file
// does not have (the same sequence runs everywhere in self-access.test.mjs,
// with that test's reader).
test('a first policy written by access-cli under a running server keeps the health command at 0; other addresses make the server end with its restart code', { timeout: 90000, skip: process.platform === 'win32' ? 'the server reads no policy file on Windows' : false }, async t => {
  const { realpath, readFile } = await import('node:fs/promises');
  const directory = await realpath(await mkdtemp(join(tmpdir(), 'multitap-container-health-'))), port = await freePort();
  const cli = fileURLToPath(new URL('../src/access-cli.mjs', import.meta.url)), here = 'http://192.0.2.10:' + port, outside = 'http://203.0.113.7:' + port;
  const env = { PATH: process.env.PATH, MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port),
    MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_HTTP_ORIGIN: here + ',' + outside, MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TLS_MODE: 'proxy', MULTITAP_DOCKER_UPDATE_FEED: 'off' };
  const run = async (file, ...argv) => {
    const child = spawn(process.execPath, [file, ...argv], { env, stdio: ['ignore', 'pipe', 'ignore'] });
    let text = ''; child.stdout.on('data', chunk => { text += chunk; });
    const timer = setTimeout(() => child.kill(), 20000);
    try { return { code: await ended(child), text }; } finally { clearTimeout(timer); }
  };
  const server = spawn(process.execPath, [entry], { env, stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  server.stdout.on('data', chunk => { output += chunk; }); server.stderr.on('data', chunk => { output += chunk; });
  const stopped = ended(server);
  t.after(async () => { server.kill(); await stopped; await rm(directory, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); });
  for (const end = Date.now() + 30000; !output.includes('"listeners"');) { assert.ok(Date.now() < end && server.exitCode === null, 'the server did not start:\n' + output); await new Promise(resolve => setTimeout(resolve, 50)); }
  assert.equal((await run(health, '--container-health')).code, 0);
  // The first policy, with its installation ID, while the server runs on its .env lines.
  const on = await run(cli, 'public-ip', 'on', '--address', '203.0.113.7');
  assert.equal(on.code, 0, on.text); assert.match(on.text, /^ok=1\nchanged=1\nsource=saved\n/); assert.match(on.text, new RegExp('\\nfollowed=' + outside.replaceAll('.', '\\.') + '\\n'));
  assert.match(await readFile(join(directory, 'access', 'identity'), 'utf8'), /^[a-f0-9]{64}\n$/);
  for (let index = 0; index < 3; index++) assert.equal((await run(health, '--container-health')).code, 0, 'check ' + (index + 1));
  assert.equal(server.exitCode, null, 'still the same process');
  // Other addresses: nobody restarts it, so it ends by itself with the code the entrypoint starts it again on.
  assert.equal((await run(cli, 'set', here)).code, 0);
  assert.equal(await stopped, 75);
  assert.match(output, /"event":"server_access_policy_changed"/);
});
