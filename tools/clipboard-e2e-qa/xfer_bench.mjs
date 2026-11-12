// Clipboard paste throughput on loopback (README.md, "Throughput benchmark"): the real Host ClipboardWorker (clipboard_e2e_host.exe
// --quiet) and the real relay (relay_fixture.mjs on any server tree), with this script as a fast stand-in for the PC's client. It
// pastes one way and prints one JSON line: MB/s, files/s and the CPU time the Host, the relay and this stand-in spent.
//
//   node xfer_bench.mjs --server <server dir> --host-exe <clipboard_e2e_host.exe> --work <new folder named run-...>
//        [--case vm2pc|pc2vm] [--files N] [--size BYTES] [--discard] [--no-pullqueue] [--ack-chunks 4|22]
//
// vm2pc: the VM copied the files (one file: a sparse file; several: real files in one folder) and this script pulls them the way the
//        client's paste does (pull queue, AckPolicy acknowledgements, SHA-256 checked).
// pc2vm: this script offers the files and serves the Host's pulls from memory; the Host pastes them like Explorer (FileContents
//        streams, 64 KiB reads) into the work folder, or reads and drops them with --discard.
// Both clipboards are the helpers' in-memory stand-ins: no real clipboard is read or written. Absolute numbers depend on the machine's
// load; compare runs made under the same conditions.
import { spawn } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createInterface } from 'node:readline';

const args = process.argv.slice(2);
const option = (name, fallback) => { const at = args.indexOf(name); return at < 0 ? fallback : args[at + 1]; };
const serverDir = resolve(option('--server', ''));
const hostExe = resolve(option('--host-exe', ''));
const work = resolve(option('--work', ''));
const which = option('--case', 'vm2pc');
const files = Number(option('--files', '1'));
const size = Number(option('--size', String(2 * 1024 ** 3)));
const discard = args.includes('--discard');
const pullQueue = !args.includes('--no-pullqueue');
// vm2pc: the receiver's acknowledgement rhythm (AckPolicy in pull_impl.inl): every N chunks or N*45 KiB. A receiver without a pull queue acks every
// 4 chunks / 128 KiB; a pull-queue receiver every 22 chunks.
const ackChunks = Number(option('--ack-chunks', '4'));
if (!basename(work).startsWith('run-') || existsSync(work)) throw new Error('--work must be a new folder named run-...');
if (!['vm2pc', 'pc2vm'].includes(which) || !(files >= 1) || !(size >= 1)) throw new Error('bad --case/--files/--size');
mkdirSync(work, { recursive: true });
const here = dirname(fileURLToPath(import.meta.url));
const { WebSocket } = await import(pathToFileURL(join(serverDir, 'node_modules', 'ws', 'wrapper.mjs')).href);
const lines = (child, onLine) => createInterface({ input: child.stdout }).on('line', line => { if (line.trim()) onLine(JSON.parse(line)); });

// ---- the relay (its own process, so its CPU time is its own) ----
const relay = spawn(process.execPath, [join(here, 'relay_fixture.mjs'), '--server', serverDir], { stdio: ['pipe', 'pipe', 'inherit'] });
let relayCpuWaiter = null; const closures = [];
const info = await new Promise((ok, fail) => {
  lines(relay, value => {
    if (value.ready) ok(value);
    else if (value.event === 'cpu' && relayCpuWaiter) { relayCpuWaiter(value); relayCpuWaiter = null; }
    else if (value.event === 'pairClosed') closures.push(value.reason);
  });
  relay.once('exit', code => fail(new Error('relay exited ' + code)));
});
const relayCpu = () => new Promise(ok => { relayCpuWaiter = ok; relay.stdin.write(JSON.stringify({ cmd: 'cpu' }) + '\n'); });

// ---- the Host helper ----
const host = spawn(hostExe, ['--url', info.hostUrl, '--token', info.hostToken, '--session', info.sessionId, '--root', work, '--quiet', ...(pullQueue ? [] : ['--no-pullqueue'])], { stdio: ['pipe', 'pipe', 'inherit'] });
const pending = new Map(); let nextAsk = 1; let started;
const hostStarted = new Promise(ok => { started = ok; });
lines(host, value => {
  if (value.event === 'started') started();
  else if (typeof value.id === 'number' && pending.has(value.id)) { const p = pending.get(value.id); pending.delete(value.id); if ('err' in value) p.fail(new Error(JSON.stringify(value.err))); else p.ok(value.ok); }
});
const ask = body => new Promise((ok, fail) => { const id = nextAsk++; pending.set(id, { ok, fail }); host.stdin.write(JSON.stringify({ id, ...body }) + '\n'); });
await hostStarted;

// ---- this script: the client's end of the clipboard channel ----
const headers = { Authorization: 'Bearer ' + info.clientToken, 'X-Multitap-Clipboard-Limits': '2', 'X-Multitap-Clipboard-Formats': '2', 'X-Multitap-Clipboard-Pull': '1', 'X-Multitap-Clipboard-Scale': '1', 'X-Multitap-Clipboard-OnPaste': '1', ...(pullQueue ? { 'X-Multitap-Clipboard-PullQueue': '1' } : {}) };
const ws = new WebSocket(info.clientUrl, { perMessageDeflate: false, headers });
const inbox = []; let waiter = null; let data = null;
ws.on('message', bytes => {
  const value = JSON.parse(bytes.toString());
  if (data && data(value)) return;
  if (waiter && waiter.test(value)) { const w = waiter; waiter = null; w.ok(value); } else inbox.push(value);
});
const take = test => {
  const at = inbox.findIndex(test); if (at >= 0) return Promise.resolve(inbox.splice(at, 1)[0]);
  return new Promise((ok, fail) => { waiter = { test, ok }; setTimeout(() => fail(new Error('no message; relay closures: ' + closures.join(','))), 120000).unref(); });
};
await new Promise((ok, fail) => { ws.once('open', ok); ws.once('error', fail); });
const ready = await take(value => value.type === 'clipboard.ready');
if (pullQueue && ready.clipboardPullQueue !== 'pull-queue-4-v1') throw new Error('the relay did not negotiate the pull queue');
const send = value => ws.send(typeof value === 'string' ? value : JSON.stringify(value));
const prefix = randomBytes(16).toString('hex'); let counter = 0;
const requestId = () => `${prefix}-${(++counter).toString(16).padStart(16, '0')}`;
const depth = pullQueue ? 4 : 1, windowBytes = pullQueue ? 4 * 1024 * 1024 : 1024 * 1024, windowUnits = pullQueue ? 128 : 32;
const chunkBytes = ready.limits.chunkBytes;
const mtime = 1761500000123;

async function cpu() { const [h, r] = await Promise.all([ask({ cmd: 'cpu' }), relayCpu()]); const self = process.cpuUsage(); return { host: h.userMs + h.kernelMs, relay: r.userMs + r.systemMs, client: (self.user + self.system) / 1000 }; }

// The VM copied the files; this script pulls every file like the client's paste and checks each hash.
async function vm2pc() {
  const src = join(work, 'src'); let paths;
  if (files === 1) { const path = join(src, 'big.bin'); await ask({ cmd: 'makeSparse', path, size, tail: 'END' }); paths = [path]; }
  else {
    const folder = join(src, 'tree'); mkdirSync(folder, { recursive: true }); const block = randomBytes(size);
    for (let i = 0; i < files; i++) { block.writeUInt32LE(i, 0); writeFileSync(join(folder, `f${String(i).padStart(5, '0')}.bin`), block); }
    paths = [folder];
  }
  await ask({ cmd: 'setFiles', paths });
  const id = requestId();
  send({ type: 'clipboard.read', requestId: id });
  const offer = await take(value => value.requestId === id && (value.type === 'clipboard.offer' || value.type === 'clipboard.error'));
  if (offer.type !== 'clipboard.offer') throw new Error('the copy was not offered: ' + offer.reason);
  const entries = [];
  while (entries.length < offer.entries) entries.push(...(await take(value => value.type === 'clipboard.entries' && value.requestId === id)).entries);
  send({ type: 'clipboard.applied', requestId: id });
  await take(value => value.type === 'clipboard.complete' && value.requestId === id);
  const sizes = entries.filter(entry => entry.kind === 'file').map(entry => entry.size);
  const before = await cpu(), start = process.hrtime.bigint();
  const bytes = await new Promise((ok, fail) => {
    let next = 0, done = 0, n = 0, total = 0; const live = [];
    const pump = () => {
      while (live.length < depth && next < sizes.length) {
        const index = next++; if (sizes[index] === 0) { done++; continue; }
        const pull = { id: `pullc-${++n}`, index, size: sizes[index], got: 0, chunks: 0, unacked: 0, hash: createHash('sha256') };
        live.push(pull); send({ type: 'clipboard.pull', requestId: pull.id, offerId: id, index, offset: 0 });
      }
      if (done === sizes.length) { data = null; ok(total); }
    };
    data = value => {
      const pull = live.find(candidate => candidate.id === value.requestId); if (!pull) return false;
      if (value.type === 'clipboard.chunk') {
        const chunk = Buffer.from(value.data, 'base64');
        if (value.offset !== pull.got) fail(new Error('out of order'));
        pull.hash.update(chunk); pull.got += chunk.length; total += chunk.length;
        // AckPolicy (ack_window.hpp) of a windowed receiver: every --ack-chunks chunks, or that many 45 KiB chunks' bytes
        // (4: the released every four chunks or 128 KiB).
        pull.chunks++; pull.unacked += chunk.length;
        if (pull.chunks >= ackChunks || pull.unacked >= (ackChunks === 4 ? 128 * 1024 : ackChunks * 46080)) { pull.chunks = 0; pull.unacked = 0; send({ type: 'clipboard.chunk.ack', requestId: pull.id, index: pull.index, offset: pull.got }); }
      } else if (value.type === 'clipboard.item.end') {
        if (pull.got !== pull.size || pull.hash.digest('hex') !== value.sha256) return fail(new Error('hash or size mismatch')), true;
        live.splice(live.indexOf(pull), 1); done++; pump();
      } else fail(new Error(`${value.type} ${value.reason ?? ''}; relay closures: ${closures.join(',')}`));
      return true;
    };
    pump();
  });
  return { bytes, seconds: Number(process.hrtime.bigint() - start) / 1e9, before, after: await cpu() };
}

// This script offers the files and serves the Host's pulls from memory; the Host pastes them like Explorer.
async function pc2vm() {
  const entries = files === 1 ? [{ path: 'big.bin', kind: 'file', size, attrs: 32, mtime }]
    : [{ path: 'tree', kind: 'dir', attrs: 0, mtime }, ...Array.from({ length: files }, (_, i) => ({ path: `tree/f${String(i).padStart(5, '0')}.bin`, kind: 'file', size, attrs: 32, mtime }))];
  const sizes = entries.filter(entry => entry.kind === 'file').map(entry => entry.size);
  const id = requestId();
  send({ type: 'clipboard.offer', requestId: id, format: 'tree', entries: entries.length, files, bytes: files * size, tops: 1, skipped: 0 });
  for (let at = 0; at < entries.length; at += 200) send({ type: 'clipboard.entries', requestId: id, start: at, entries: entries.slice(at, at + 200) });
  await take(value => value.type === 'clipboard.complete' && value.requestId === id);
  // Every file is the same pseudo-random block repeated (served at chunk-aligned offsets: each chunk is a prefix of the block).
  const block = randomBytes(chunkBytes), blockText = block.toString('base64'); const queue = []; let failure = null;
  const serve = () => {
    while (queue.length) {
      const pull = queue[0];
      while (pull.sent < pull.size) {
        const n = Math.min(chunkBytes, pull.size - pull.sent);
        if (pull.units.length >= windowUnits || pull.inflight + n > windowBytes) return;
        const piece = n === chunkBytes ? block : block.subarray(0, n);
        pull.hash.update(piece);
        send(`{"type":"clipboard.chunk","requestId":"${pull.id}","index":${pull.index},"offset":${pull.sent},"data":"${n === chunkBytes ? blockText : piece.toString('base64')}"}`);
        pull.sent += n; pull.units.push({ end: pull.sent, bytes: n }); pull.inflight += n;
      }
      if (pull.units.length >= windowUnits) return;
      send({ type: 'clipboard.item.end', requestId: pull.id, index: pull.index, sha256: pull.hash.digest('hex') });
      queue.shift();
    }
  };
  data = value => {
    if (value.type === 'clipboard.pull') {
      if (value.offset !== 0) { failure = 'a pull from an offset'; return true; }
      queue.push({ id: value.requestId, index: value.index, size: sizes[value.index], sent: 0, units: [], inflight: 0, hash: createHash('sha256') }); serve(); return true;
    }
    if (!/^pullh-/.test(value.requestId ?? '')) return false;
    const pull = queue.find(candidate => candidate.id === value.requestId);
    if (value.type === 'clipboard.chunk.ack') { if (pull) { while (pull.units.length && pull.units[0].end <= value.offset) pull.inflight -= pull.units.shift().bytes; serve(); } return true; }
    if (value.type === 'clipboard.cancel' || value.type === 'clipboard.error') { if (pull) queue.splice(queue.indexOf(pull), 1); serve(); return true; }
    return true;
  };
  const before = await cpu(), start = process.hrtime.bigint();
  const result = await ask({ cmd: 'paste', dest: join(work, 'paste'), discard });
  const seconds = Number(process.hrtime.bigint() - start) / 1e9;
  if (failure) throw new Error(failure);
  return { bytes: result.bytes, seconds, before, after: await cpu() };
}

let outcome, error;
try { outcome = which === 'vm2pc' ? await vm2pc() : await pc2vm(); } catch (failure) { error = failure; }
try { await ask({ cmd: 'quit' }); } catch {}
ws.close(); relay.stdin.end();
await new Promise(ok => setTimeout(ok, 300));
rmSync(work, { recursive: true, force: true });
if (error) { console.error(error.message); process.exit(1); }
const { bytes, seconds, before, after } = outcome;
const spent = Object.fromEntries(Object.keys(before).map(key => [key, Math.round(after[key] - before[key])]));
console.log(JSON.stringify({ case: which, pullQueue, discard, ackChunks, files, size, bytes, seconds: Number(seconds.toFixed(3)), MBps: Number((bytes / 1e6 / seconds).toFixed(1)), filesPerSecond: Math.round(files / seconds), cpuMs: spent, relayClosures: closures }));
process.exit(0);
