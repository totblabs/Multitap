// Starts the broker relay of a chosen server tree (server/ of this checkout, or a `git show`n older release) on a
// loopback port with an in-memory store, signs in alice, connects a stand-in Host control channel and a media
// socket (a clipboard pair needs a current stream session), and prints ONE JSON line describing the clipboard
// endpoints for the real client/Host helpers. Everything else runs in the helper processes.
//
//   node relay_fixture.mjs --server <dir with src/ and node_modules/> [--legacy] [--policy <json>]
//
// The session uses the clipboard-sync-v1 protocol like every released client; --legacy connects a Host that does not announce
// clipboardSync (random request ids instead of sequence ids).
//
// stdout: {"ready":true,"relayVersion":..,"sessionId":..,"clientUrl":..,"clientToken":..,"hostUrl":..,"hostToken":..,"legacy":..}
// later lines: {"event":"pairClosed","reason":..} when the relay ends the clipboard pair (its reason is the evidence of
// a protocol disagreement between a peer and this relay version). The process ends when stdin closes.
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createInterface } from 'node:readline';

const args = process.argv.slice(2);
const option = name => { const at = args.indexOf(name); return at < 0 ? undefined : args[at + 1]; };
const serverDir = resolve(option('--server') ?? '');
const legacy = args.includes('--legacy');
const policy = option('--policy') ? JSON.parse(option('--policy')) : { textToRemote: true, textToLocal: true, filesToRemote: true, filesToLocal: true };
const load = file => import(pathToFileURL(join(serverDir, file)).href);
const { WebSocket } = await import(pathToFileURL(join(serverDir, 'node_modules', 'ws', 'wrapper.mjs')).href);
const { Relay } = await load('src/relay.mjs');
const { Store } = await load('src/store.mjs');
const { newToken, tokenHash } = await load('src/security.mjs');
const relayVersion = JSON.parse(readFileSync(join(serverDir, 'package.json'), 'utf8')).version;

const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, clipboard: true, clipboardSync: !legacy };
const store = new Store(':memory:');
const user = store.addUser({ name: 'alice', passwordHash: 'fixture-only' });
const clientToken = newToken();
store.createSession(store.findUser('alice'), tokenHash(clientToken), Date.now(), 3600000);
store.syncProxmox([{ vmid: 810, node: 'fixture', name: 'Clipboard E2E', status: 'running' }], 'fixture');
store.assign('alice', 'pve-810');
store.updateAssignmentPolicy('alice', 'pve-810', { clipboardPolicy: policy }, 'fixture');
const code = newToken(), uuid = '12345678-1234-1234-1234-000000000810', hostToken = newToken();
store.issueEnrollment('pve-810', uuid, tokenHash(code), 'fixture');
store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'E2E host', version: '0.2.77', tokenHash: tokenHash(hostToken) });

// Idle and total limits stay at the production defaults; only the authorization sweep is quick.
const relay = new Relay({ store, publicOrigin: 'https://clipboard-e2e.example', authorizationMs: 50, heartbeatMs: 5000 });
const server = createServer();
relay.attach(server);
await new Promise(resolveListen => server.listen(0, '127.0.0.1', resolveListen));
const port = server.address().port, base = `ws://127.0.0.1:${port}`;
relay.publicOrigin = `https://127.0.0.1:${port}`;

const originalClose = relay.clipboard.closePair.bind(relay.clipboard);
relay.clipboard.closePair = (pair, reason) => {
  if (!pair.closed) process.stdout.write(JSON.stringify({ event: 'pairClosed', reason }) + '\n');
  return originalClose(pair, reason);
};

function open(url, authorization) {
  return new Promise((resolveOpen, reject) => {
    const ws = new WebSocket(url, { perMessageDeflate: false, headers: { Authorization: authorization } });
    const queue = [], waiters = [];
    ws.on('message', (bytes, binary) => {
      const value = binary ? bytes : JSON.parse(bytes.toString());
      const at = waiters.findIndex(waiter => waiter.test(value));
      if (at < 0) queue.push(value); else waiters.splice(at, 1)[0].resolve(value);
    });
    ws.once('error', reject);
    ws.take = test => {
      const at = queue.findIndex(test);
      if (at >= 0) return Promise.resolve(queue.splice(at, 1)[0]);
      return new Promise(resolveTake => waiters.push({ test, resolve: resolveTake }));
    };
    ws.once('open', () => resolveOpen(ws));
  });
}

const host = await open(`${base}/api/v1/hosts/channel`, `Host ${hostToken}`);
host.send(JSON.stringify({ type: 'host.capabilities', capabilities: caps }));
await host.take(value => value.type === 'host.capabilities.ack');
// relay.connect's parameter list changed between releases: clipboardProtocol goes into the position its own signature names.
const connectParameters = relay.connect.toString().slice(relay.connect.toString().indexOf('(') + 1, relay.connect.toString().indexOf(')')).split(',').map(part => part.split('=')[0].trim());
const clipboardAt = connectParameters.indexOf('clipboardProtocol');
if (clipboardAt < 4) throw new Error('relay.connect has no clipboardProtocol parameter');
const connectArgs = [{ id: user }, tokenHash(clientToken), 'pve-810', 'stream'];
while (connectArgs.length < clipboardAt) connectArgs.push(undefined);
connectArgs.push('clipboard-sync-v1');
const connection = relay.connect(...connectArgs);
const media = await open(base + new URL(connection.webSocketUrl).pathname, `Bearer ${clientToken}`);
const start = await host.take(value => value.type === 'session.start');
void media;
process.stdout.write(JSON.stringify({
  ready: true, relayVersion, sessionId: connection.sessionId ?? start.sessionId, legacy,
  clientUrl: base + new URL(connection.clipboardWebSocketUrl).pathname, clientToken,
  hostUrl: base + new URL(start.clipboardUrl).pathname, hostToken,
}) + '\n');
// Test-only assignment changes use the same Store API as administration. The
// relay's normal authorization sweep must revoke an in-flight paste itself.
createInterface({ input: process.stdin }).on('line', line => {
  const value = JSON.parse(line);
  // The relay process's own CPU time (the throughput benchmark, xfer_bench.mjs).
  if (value.cmd === 'cpu') { const { user, system } = process.cpuUsage(); process.stdout.write(JSON.stringify({ event: 'cpu', userMs: user / 1000, systemMs: system / 1000 }) + '\n'); return; }
  if (value.cmd !== 'policy' || !value.policy ||
      Object.keys(value.policy).sort().join(',') !== 'filesToLocal,filesToRemote,textToLocal,textToRemote' ||
      !Object.values(value.policy).every(item => typeof item === 'boolean')) throw new Error('Invalid fixture policy');
  store.updateAssignmentPolicy('alice', 'pve-810', { clipboardPolicy: value.policy }, 'fixture');
});
process.stdin.on('end', () => process.exit(0));
